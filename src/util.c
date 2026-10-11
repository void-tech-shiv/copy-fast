#include "copyfast.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_cancel_requested = 0;

static struct {
    pthread_mutex_t lock;
    uint64_t total;
    uint64_t done;
    struct timespec start;
    struct timespec last_print;
    bool quiet;
    bool initialized;
} pg = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void on_signal(int sig) {
    (void)sig;
    g_cancel_requested = 1;
}

int install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

static double sec_since(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

void progress_init(uint64_t total, bool quiet) {
    pthread_mutex_lock(&pg.lock);
    pg.total = total;
    pg.done = 0;
    pg.quiet = quiet;
    clock_gettime(CLOCK_MONOTONIC, &pg.start);
    pg.last_print = pg.start;
    pg.initialized = true;
    pthread_mutex_unlock(&pg.lock);
}

void progress_add(uint64_t bytes) {
    pthread_mutex_lock(&pg.lock);
    pg.done += bytes;
    if (pg.done > pg.total && pg.total > 0) pg.done = pg.total;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double since = sec_since(&pg.last_print, &now);
    if (!pg.quiet && since >= 0.2) {
        double elapsed = sec_since(&pg.start, &now);
        double pct = pg.total ? (100.0 * (double)pg.done / (double)pg.total) : 100.0;
        double mib = (double)pg.done / (1024.0 * 1024.0);
        double speed = elapsed > 0 ? mib / elapsed : 0.0;
        fprintf(stderr, "\rCopyFast: %6.2f%%  %.1f MiB  %.1f MiB/s", pct, mib, speed);
        fflush(stderr);
        pg.last_print = now;
    }
    pthread_mutex_unlock(&pg.lock);
}

void progress_finish(bool success) {
    pthread_mutex_lock(&pg.lock);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = sec_since(&pg.start, &now);
    double mib = (double)pg.done / (1024.0 * 1024.0);
    double speed = elapsed > 0 ? mib / elapsed : 0.0;
    if (!pg.quiet) {
        fprintf(stderr, "\r%-72s\r", "");
        fprintf(stderr, "CopyFast: %s | %.1f MiB processed | %.2f s | %.1f MiB/s\n",
                success ? "complete" : (g_cancel_requested ? "interrupted" : "failed"),
                mib, elapsed, speed);
    }
    pthread_mutex_unlock(&pg.lock);
}

void progress_note(const char *fmt, ...) {
    pthread_mutex_lock(&pg.lock);
    if (!pg.quiet) {
        fprintf(stderr, "\r%-72s\r", "");
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&pg.lock);
}

double progress_elapsed_seconds(void) {
    pthread_mutex_lock(&pg.lock);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double v = sec_since(&pg.start, &now);
    pthread_mutex_unlock(&pg.lock);
    return v;
}

uint64_t progress_total_done(void) {
    pthread_mutex_lock(&pg.lock);
    uint64_t v = pg.done;
    pthread_mutex_unlock(&pg.lock);
    return v;
}

int preserve_fd_metadata(int fd, const struct stat *st) {
    if (fchmod(fd, st->st_mode & 07777) != 0) return -1;
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    if (futimens(fd, ts) != 0) return -1;
    return 0;
}

int verify_files(const char *src, const char *dst, char src_hex[65], char dst_hex[65]) {
    unsigned char a[32], b[32];
    if (sha256_file(src, a) != 0) return -1;
    if (sha256_file(dst, b) != 0) return -1;
    sha256_hex(a, src_hex);
    sha256_hex(b, dst_hex);
    return memcmp(a, b, sizeof(a)) == 0 ? 0 : 1;
}

int check_same_file(const char *src, const char *dst) {
    struct stat a, b;
    if (stat(src, &a) != 0) return -1;
    if (stat(dst, &b) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    return (a.st_dev == b.st_dev && a.st_ino == b.st_ino) ? 1 : 0;
}

ssize_t full_pread(int fd, void *buf, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = pread(fd, (char *)buf + done, count - done, offset + (off_t)done);
        if (n > 0) done += (size_t)n;
        else if (n == 0) break;
        else if (errno != EINTR) return -1;
    }
    return (ssize_t)done;
}

ssize_t full_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = pwrite(fd, (const char *)buf + done, count - done, offset + (off_t)done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno != EINTR) return -1;
    }
    return (ssize_t)done;
}

static char *resume_path(const char *dst, const char *suffix) {
    size_t n = strlen(dst) + strlen(suffix) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s%s", dst, suffix);
    return p;
}

int resume_load(const char *dst, const struct stat *src_st, off_t *offset) {
    *offset = 0;
    char *path = resume_path(dst, ".copyfast.resume");
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    long long size = -1, sec = -1, nsec = -1, off = -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        (void)sscanf(line, "size=%lld", &size);
        (void)sscanf(line, "mtime_sec=%lld", &sec);
        (void)sscanf(line, "mtime_nsec=%lld", &nsec);
        (void)sscanf(line, "offset=%lld", &off);
    }
    fclose(f);

    if (size != (long long)src_st->st_size ||
        sec != (long long)src_st->st_mtim.tv_sec ||
        nsec != (long long)src_st->st_mtim.tv_nsec ||
        off < 0 || off > size) {
        return 0;
    }

    struct stat dst_st;
    if (stat(dst, &dst_st) != 0 || dst_st.st_size < off) {
        return 0;
    }

    *offset = (off_t)off;
    return 1;
}

int resume_save(const char *dst, const struct stat *src_st, off_t offset) {
    char *path = resume_path(dst, ".copyfast.resume");
    char *tmp = resume_path(dst, ".copyfast.resume.tmp");
    if (!path || !tmp) { free(path); free(tmp); errno = ENOMEM; return -1; }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(path); free(tmp); return -1; }
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "size=%lld\nmtime_sec=%lld\nmtime_nsec=%lld\noffset=%lld\n",
        (long long)src_st->st_size,
        (long long)src_st->st_mtim.tv_sec,
        (long long)src_st->st_mtim.tv_nsec,
        (long long)offset);
    ssize_t w = write(fd, buf, (size_t)n);
    int saved = errno;
    if (w == n) (void)fsync(fd);
    if (close(fd) != 0 && w == n) { w = -1; saved = errno; }
    if (w != n) {
        unlink(tmp); free(path); free(tmp); errno = saved; return -1;
    }
    if (rename(tmp, path) != 0) {
        saved = errno; unlink(tmp); free(path); free(tmp); errno = saved; return -1;
    }
    free(path); free(tmp);
    return 0;
}

void resume_remove(const char *dst) {
    char *path = resume_path(dst, ".copyfast.resume");
    char *tmp = resume_path(dst, ".copyfast.resume.tmp");
    if (path) { (void)unlink(path); free(path); }
    if (tmp) { (void)unlink(tmp); free(tmp); }
}
