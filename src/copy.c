#include "copyfast.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { CHUNK_DATA = 0, CHUNK_END, CHUNK_CANCEL, CHUNK_ERROR } chunk_kind_t;

typedef struct {
    unsigned char *data;
    size_t len;
    off_t offset;
    chunk_kind_t kind;
} chunk_t;

typedef struct {
    int src_fd;
    int dst_fd;
    const copy_job_t *job;
    const options_t *opts;
    off_t start_offset;
    chunk_t *slots;
    size_t head;
    size_t tail;
    sem_t empty;
    sem_t full;
    atomic_int writer_failed;
    int reader_errno;
    int writer_errno;
    uint64_t written_data;
} pipeline_t;

static int open_pair(const copy_job_t *job, bool truncate_dst, int *src_fd, int *dst_fd) {
    int same = check_same_file(job->src, job->dst);
    if (same < 0) return -1;
    if (same > 0) { errno = EINVAL; return -1; }

    int s = open(job->src, O_RDONLY | O_CLOEXEC);
    if (s < 0) return -1;
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    if (truncate_dst) flags |= O_TRUNC;
    int d = open(job->dst, flags, 0600);
    if (d < 0) { int e = errno; close(s); errno = e; return -1; }
    (void)posix_fadvise(s, 0, 0, POSIX_FADV_SEQUENTIAL);
    (void)posix_fadvise(d, 0, 0, POSIX_FADV_SEQUENTIAL);
    *src_fd = s;
    *dst_fd = d;
    return 0;
}

static int wait_empty(pipeline_t *p, bool respect_cancel) {
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (sem_timedwait(&p->empty, &ts) == 0) return 0;
        if (errno == EINTR || errno == ETIMEDOUT) {
            if (atomic_load(&p->writer_failed)) return -1;
            if (respect_cancel && g_cancel_requested) return 1;
            continue;
        }
        return -1;
    }
}

static int push_control(pipeline_t *p, chunk_kind_t kind) {
    int rc = wait_empty(p, false);
    if (rc != 0) return -1;
    chunk_t *c = &p->slots[p->tail];
    c->kind = kind;
    c->len = 0;
    c->offset = 0;
    p->tail = (p->tail + 1) % p->opts->queue_depth;
    sem_post(&p->full);
    return 0;
}

static void *reader_main(void *arg) {
    pipeline_t *p = arg;
    extent_iter_t it;
    extent_iter_init(&it, p->src_fd, p->job->st.st_size, p->start_offset);

    for (;;) {
        if (g_cancel_requested) {
            (void)push_control(p, CHUNK_CANCEL);
            return NULL;
        }
        if (atomic_load(&p->writer_failed)) return NULL;

        off_t off;
        size_t len;
        int nx = extent_next(&it, p->opts->buffer_size, &off, &len);
        if (nx == 0) {
            (void)push_control(p, CHUNK_END);
            return NULL;
        }
        if (nx < 0) {
            p->reader_errno = errno;
            (void)push_control(p, CHUNK_ERROR);
            return NULL;
        }

        int wr = wait_empty(p, true);
        if (wr == 1) {
            (void)push_control(p, CHUNK_CANCEL);
            return NULL;
        }
        if (wr != 0) return NULL;

        chunk_t *c = &p->slots[p->tail];
        ssize_t n = full_pread(p->src_fd, c->data, len, off);
        if (n < 0 || (size_t)n != len) {
            p->reader_errno = n < 0 ? errno : EIO;
            c->kind = CHUNK_ERROR;
            c->len = 0;
        } else {
            c->kind = CHUNK_DATA;
            c->len = len;
            c->offset = off;
        }
        p->tail = (p->tail + 1) % p->opts->queue_depth;
        sem_post(&p->full);
        if (c->kind == CHUNK_ERROR) return NULL;
    }
}

static void maybe_test_delay(void) {
    static long delay_us = -1;
    if (delay_us == -1) {
        const char *e = getenv("COPYFAST_TEST_DELAY_US");
        delay_us = e ? strtol(e, NULL, 10) : 0;
        if (delay_us < 0) delay_us = 0;
    }
    if (delay_us > 0) usleep((useconds_t)delay_us);
}

static void *writer_main(void *arg) {
    pipeline_t *p = arg;
    for (;;) {
        while (sem_wait(&p->full) != 0) {
            if (errno != EINTR) {
                p->writer_errno = errno;
                atomic_store(&p->writer_failed, 1);
                return NULL;
            }
        }
        chunk_t *c = &p->slots[p->head];
        p->head = (p->head + 1) % p->opts->queue_depth;

        if (c->kind == CHUNK_DATA) {
            if (full_pwrite(p->dst_fd, c->data, c->len, c->offset) != (ssize_t)c->len) {
                p->writer_errno = errno ? errno : EIO;
                atomic_store(&p->writer_failed, 1);
                sem_post(&p->empty);
                return NULL;
            }
            p->written_data += c->len;
            progress_add(c->len);
            if (p->opts->resume && resume_save(p->job->dst, &p->job->st, c->offset + (off_t)c->len) != 0) {
                p->writer_errno = errno;
                atomic_store(&p->writer_failed, 1);
                sem_post(&p->empty);
                return NULL;
            }
            maybe_test_delay();
            sem_post(&p->empty);
            continue;
        }

        chunk_kind_t kind = c->kind;
        sem_post(&p->empty);
        if (kind == CHUNK_ERROR) {
            p->writer_errno = p->reader_errno ? p->reader_errno : EIO;
        }
        return NULL;
    }
}

static int post_copy_finish(const copy_job_t *job, const options_t *opts, int dst_fd,
                            off_t start_offset, uint64_t written_data) {
    if (ftruncate(dst_fd, job->st.st_size) != 0) return -1;
    if (preserve_fd_metadata(dst_fd, &job->st) != 0) return -1;
    if (fsync(dst_fd) != 0) return -1;

    uint64_t logical_remaining = (uint64_t)(job->st.st_size - start_offset);
    if (logical_remaining > written_data) progress_add(logical_remaining - written_data);
    if (opts->resume) resume_remove(job->dst);

    if (opts->verify) {
        char a[65], b[65];
        int v = verify_files(job->src, job->dst, a, b);
        if (v < 0) return -1;
        if (v > 0) {
            progress_note("VERIFY FAILED: %s\n  source      %s\n  destination %s", job->dst, a, b);
            errno = EIO;
            return -1;
        }
    }
    return 0;
}

int copy_regular_threaded(const copy_job_t *job, const options_t *opts) {
    off_t start = 0;
    int resumed = 0;
    if (opts->resume) {
        resumed = resume_load(job->dst, &job->st, &start);
        if (resumed < 0) return -1;
    }
    bool truncate_dst = !(resumed > 0 && start > 0);

    int src_fd, dst_fd;
    if (open_pair(job, truncate_dst, &src_fd, &dst_fd) != 0) return -1;
    if (start > 0) {
        progress_add((uint64_t)start);
        progress_note("Resuming %s at %.2f MiB", job->dst, (double)start / (1024.0*1024.0));
    } else if (opts->resume && resume_save(job->dst, &job->st, 0) != 0) {
        int e = errno; close(src_fd); close(dst_fd); errno = e; return -1;
    }

    pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.src_fd = src_fd; p.dst_fd = dst_fd; p.job = job; p.opts = opts; p.start_offset = start;
    atomic_init(&p.writer_failed, 0);
    p.slots = calloc(opts->queue_depth, sizeof(*p.slots));
    if (!p.slots) { close(src_fd); close(dst_fd); return -1; }
    for (size_t i = 0; i < opts->queue_depth; ++i) {
        p.slots[i].data = malloc(opts->buffer_size);
        if (!p.slots[i].data) {
            for (size_t j = 0; j < i; ++j) free(p.slots[j].data);
            free(p.slots); close(src_fd); close(dst_fd); errno = ENOMEM; return -1;
        }
    }
    if (sem_init(&p.empty, 0, (unsigned)opts->queue_depth) != 0 || sem_init(&p.full, 0, 0) != 0) {
        int e = errno;
        for (size_t i=0;i<opts->queue_depth;++i) free(p.slots[i].data);
        free(p.slots); close(src_fd); close(dst_fd); errno=e; return -1;
    }

    pthread_t reader, writer;
    int er = pthread_create(&reader, NULL, reader_main, &p);
    int ew = er ? er : pthread_create(&writer, NULL, writer_main, &p);
    if (er || ew) {
        errno = er ? er : ew;
        if (!er) { g_cancel_requested = 1; pthread_join(reader, NULL); }
        sem_destroy(&p.empty); sem_destroy(&p.full);
        for (size_t i=0;i<opts->queue_depth;++i) free(p.slots[i].data);
        free(p.slots); close(src_fd); close(dst_fd);
        return -1;
    }

    pthread_join(reader, NULL);
    pthread_join(writer, NULL);

    int rc = 0;
    if (p.writer_errno || p.reader_errno || atomic_load(&p.writer_failed)) {
        errno = p.writer_errno ? p.writer_errno : p.reader_errno;
        rc = -1;
    } else if (g_cancel_requested) {
        rc = 1;
    } else if (post_copy_finish(job, opts, dst_fd, start, p.written_data) != 0) {
        rc = -1;
    }

    int saved = errno;
    sem_destroy(&p.empty); sem_destroy(&p.full);
    for (size_t i=0;i<opts->queue_depth;++i) free(p.slots[i].data);
    free(p.slots);
    close(src_fd);
    if (close(dst_fd) != 0 && rc == 0) { rc = -1; saved = errno; }
    errno = saved;
    return rc;
}

int copy_regular_naive(const copy_job_t *job, const options_t *opts) {
    off_t start = 0;
    int resumed = 0;
    if (opts->resume) {
        resumed = resume_load(job->dst, &job->st, &start);
        if (resumed < 0) return -1;
    }
    int src_fd, dst_fd;
    if (open_pair(job, !(resumed > 0 && start > 0), &src_fd, &dst_fd) != 0) return -1;

    if (lseek(src_fd, start, SEEK_SET) < 0 || lseek(dst_fd, start, SEEK_SET) < 0) {
        int e=errno; close(src_fd); close(dst_fd); errno=e; return -1;
    }
    if (start > 0) progress_add((uint64_t)start);
    else if (opts->resume && resume_save(job->dst, &job->st, 0) != 0) {
        int e=errno; close(src_fd); close(dst_fd); errno=e; return -1;
    }

    unsigned char *buf = malloc(opts->buffer_size);
    if (!buf) { close(src_fd); close(dst_fd); return -1; }
    off_t offset = start;
    int rc = 0;
    while (!g_cancel_requested) {
        ssize_t n = read(src_fd, buf, opts->buffer_size);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; rc=-1; break; }
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t w = write(dst_fd, buf + done, (size_t)n - done);
            if (w > 0) done += (size_t)w;
            else if (w < 0 && errno == EINTR) continue;
            else { rc=-1; break; }
        }
        if (rc != 0) break;
        offset += n;
        progress_add((uint64_t)n);
        if (opts->resume && resume_save(job->dst, &job->st, offset) != 0) { rc=-1; break; }
        maybe_test_delay();
    }
    if (g_cancel_requested && rc == 0) rc = 1;
    if (rc == 0 && post_copy_finish(job, opts, dst_fd, start,
                                    (uint64_t)(job->st.st_size - start)) != 0) rc = -1;

    int saved=errno;
    free(buf); close(src_fd);
    if (close(dst_fd) != 0 && rc==0) { rc=-1; saved=errno; }
    errno=saved;
    return rc;
}
