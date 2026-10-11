#ifndef COPYFAST_H
#define COPYFAST_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef enum {
    BACKEND_THREADED = 0,
    BACKEND_NAIVE,
    BACKEND_IO_URING
} backend_t;

typedef struct {
    backend_t backend;
    size_t buffer_size;
    size_t queue_depth;
    unsigned jobs;
    bool verify;
    bool resume;
    bool quiet;
} options_t;

typedef enum {
    JOB_REGULAR = 0,
    JOB_SYMLINK
} job_type_t;

typedef struct {
    char *src;
    char *dst;
    struct stat st;
    job_type_t type;
} copy_job_t;

typedef struct {
    char *src;
    char *dst;
    struct stat st;
} dir_meta_t;

typedef struct {
    copy_job_t *jobs;
    size_t job_count;
    size_t job_cap;
    dir_meta_t *dirs;
    size_t dir_count;
    size_t dir_cap;
    uint64_t total_bytes;
} copy_plan_t;

typedef struct {
    int fd;
    off_t size;
    off_t pos;
    off_t extent_end;
    bool dense_fallback;
    bool have_extent;
} extent_iter_t;

extern volatile sig_atomic_t g_cancel_requested;

/* plan / filesystem */
int build_copy_plan(const char *src, const char *dst, copy_plan_t *plan);
void free_copy_plan(copy_plan_t *plan);
int finalize_directories(const copy_plan_t *plan);
int copy_symlink_job(const copy_job_t *job);

/* copy engines */
int copy_regular_threaded(const copy_job_t *job, const options_t *opts);
int copy_regular_naive(const copy_job_t *job, const options_t *opts);
int copy_regular_io_uring(const copy_job_t *job, const options_t *opts);
bool io_uring_compiled(void);

/* sparse extent iterator */
void extent_iter_init(extent_iter_t *it, int fd, off_t size, off_t start);
int extent_next(extent_iter_t *it, size_t max_len, off_t *offset, size_t *length);

/* shared helpers */
int preserve_fd_metadata(int fd, const struct stat *st);
int verify_files(const char *src, const char *dst, char src_hex[65], char dst_hex[65]);
int check_same_file(const char *src, const char *dst);
ssize_t full_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t full_pwrite(int fd, const void *buf, size_t count, off_t offset);

/* resume */
int resume_load(const char *dst, const struct stat *src_st, off_t *offset);
int resume_save(const char *dst, const struct stat *src_st, off_t offset);
void resume_remove(const char *dst);

/* progress/logging */
void progress_init(uint64_t total, bool quiet);
void progress_add(uint64_t bytes);
void progress_finish(bool success);
void progress_note(const char *fmt, ...);
double progress_elapsed_seconds(void);
uint64_t progress_total_done(void);

/* signals */
int install_signal_handlers(void);

/* SHA-256 */
int sha256_file(const char *path, unsigned char out[32]);
void sha256_hex(const unsigned char hash[32], char hex[65]);

#endif
