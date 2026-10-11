#ifdef HAVE_LIBURING
#define _GNU_SOURCE
#include "copyfast.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { OP_READ = 1, OP_WRITE = 2 } op_kind_t;
typedef struct {
    op_kind_t kind;
    unsigned char *buf;
    size_t len;
    size_t done;
    size_t read_remaining;
    off_t offset;
    bool busy;
} ur_op_t;

bool io_uring_compiled(void) { return true; }

static int open_pair_uring(const copy_job_t *job, int *s, int *d) {
    int same = check_same_file(job->src, job->dst);
    if (same != 0) { if (same > 0) errno = EINVAL; return -1; }
    *s = open(job->src, O_RDONLY | O_CLOEXEC);
    if (*s < 0) return -1;
    *d = open(job->dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (*d < 0) { int e=errno; close(*s); errno=e; return -1; }
    return 0;
}

static int submit_read(struct io_uring *ring, ur_op_t *op, int src, off_t off, size_t len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    op->kind = OP_READ; op->len = len; op->done = 0; op->read_remaining = 0; op->offset = off; op->busy = true;
    io_uring_prep_read(sqe, src, op->buf, len, off);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

static int submit_write(struct io_uring *ring, ur_op_t *op, int dst) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    op->kind = OP_WRITE;
    io_uring_prep_write(sqe, dst, op->buf + op->done, op->len - op->done,
                        op->offset + (off_t)op->done);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

int copy_regular_io_uring(const copy_job_t *job, const options_t *opts) {
    if (opts->resume) {
        progress_note("io_uring backend does not support --resume; use --backend threaded for resumable transfers.");
        errno = ENOTSUP;
        return -1;
    }

    int src=-1, dst=-1;
    if (open_pair_uring(job, &src, &dst) != 0) return -1;
    (void)posix_fadvise(src, 0, 0, POSIX_FADV_SEQUENTIAL);

    unsigned qd = (unsigned)(opts->queue_depth < 2 ? 2 : opts->queue_depth);
    struct io_uring ring;
    int qret = io_uring_queue_init(qd * 2, &ring, 0);
    if (qret < 0) {
        close(src); close(dst); errno = -qret; return -1;
    }

    ur_op_t *ops = calloc(qd, sizeof(*ops));
    if (!ops) { io_uring_queue_exit(&ring); close(src); close(dst); return -1; }
    for (unsigned i=0;i<qd;++i) {
        ops[i].buf = malloc(opts->buffer_size);
        if (!ops[i].buf) {
            for (unsigned j=0;j<i;++j) free(ops[j].buf);
            free(ops); io_uring_queue_exit(&ring); close(src); close(dst); errno=ENOMEM; return -1;
        }
    }

    extent_iter_t it;
    extent_iter_init(&it, src, job->st.st_size, 0);
    unsigned inflight = 0;
    bool no_more = false;
    uint64_t written = 0;
    int rc = 0;

    while ((!no_more || inflight > 0) && !g_cancel_requested) {
        for (unsigned i=0; i<qd && !no_more; ++i) {
            if (ops[i].busy) continue;
            off_t off; size_t len;
            int nx = extent_next(&it, opts->buffer_size, &off, &len);
            if (nx == 0) { no_more = true; break; }
            if (nx < 0 || submit_read(&ring, &ops[i], src, off, len) != 0) { rc=-1; no_more=true; break; }
            inflight++;
        }
        if (rc != 0) break;
        if (io_uring_submit(&ring) < 0) { rc=-1; break; }
        if (inflight == 0) break;

        struct io_uring_cqe *cqe;
        int w = io_uring_wait_cqe(&ring, &cqe);
        if (w < 0) { errno = -w; rc=-1; break; }
        ur_op_t *op = io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        if (!op || res < 0) { if (res < 0) errno=-res; rc=-1; break; }

        if (op->kind == OP_READ) {
            if (res == 0) { errno=EIO; rc=-1; break; }
            size_t requested = op->len;
            op->len = (size_t)res;
            op->done = 0;
            op->read_remaining = requested - (size_t)res;
            if (submit_write(&ring, op, dst) != 0 || io_uring_submit(&ring) < 0) { rc=-1; break; }
        } else {
            op->done += (size_t)res;
            if (op->done < op->len) {
                if (submit_write(&ring, op, dst) != 0 || io_uring_submit(&ring) < 0) { rc=-1; break; }
            } else {
                written += op->len;
                progress_add(op->len);
                if (op->read_remaining > 0) {
                    off_t next_off = op->offset + (off_t)op->len;
                    size_t remain = op->read_remaining;
                    if (submit_read(&ring, op, src, next_off, remain) != 0 || io_uring_submit(&ring) < 0) { rc=-1; break; }
                } else {
                    op->busy = false;
                    inflight--;
                }
            }
        }
    }

    if (g_cancel_requested && rc == 0) rc = 1;
    if (rc == 0) {
        if (ftruncate(dst, job->st.st_size) != 0 || preserve_fd_metadata(dst, &job->st) != 0 || fsync(dst) != 0) rc=-1;
        if (rc == 0 && (uint64_t)job->st.st_size > written) progress_add((uint64_t)job->st.st_size - written);
        if (rc == 0 && opts->verify) {
            char a[65], b[65];
            int v=verify_files(job->src,job->dst,a,b);
            if (v != 0) { if (v>0) progress_note("VERIFY FAILED: %s",job->dst); errno=EIO; rc=-1; }
        }
    }

    int saved=errno;
    for (unsigned i=0;i<qd;++i) free(ops[i].buf);
    free(ops); io_uring_queue_exit(&ring); close(src); close(dst); errno=saved;
    return rc;
}
#endif
