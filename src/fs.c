#include "copyfast.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *path_join(const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b);
    bool slash = al > 0 && a[al-1] != '/';
    char *out = malloc(al + (slash ? 1 : 0) + bl + 1);
    if (!out) return NULL;
    memcpy(out, a, al);
    size_t p = al;
    if (slash) out[p++] = '/';
    memcpy(out + p, b, bl + 1);
    return out;
}

static int add_job(copy_plan_t *plan, const char *src, const char *dst,
                   const struct stat *st, job_type_t type) {
    if (plan->job_count == plan->job_cap) {
        size_t nc = plan->job_cap ? plan->job_cap * 2 : 32;
        copy_job_t *p = realloc(plan->jobs, nc * sizeof(*p));
        if (!p) return -1;
        plan->jobs = p;
        plan->job_cap = nc;
    }
    copy_job_t *j = &plan->jobs[plan->job_count++];
    j->src = strdup(src);
    j->dst = strdup(dst);
    j->st = *st;
    j->type = type;
    if (!j->src || !j->dst) return -1;
    if (type == JOB_REGULAR) plan->total_bytes += (uint64_t)st->st_size;
    return 0;
}

static int add_dir(copy_plan_t *plan, const char *src, const char *dst, const struct stat *st) {
    if (plan->dir_count == plan->dir_cap) {
        size_t nc = plan->dir_cap ? plan->dir_cap * 2 : 16;
        dir_meta_t *p = realloc(plan->dirs, nc * sizeof(*p));
        if (!p) return -1;
        plan->dirs = p;
        plan->dir_cap = nc;
    }
    dir_meta_t *d = &plan->dirs[plan->dir_count++];
    d->src = strdup(src);
    d->dst = strdup(dst);
    d->st = *st;
    if (!d->src || !d->dst) return -1;
    return 0;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) return -1;
    return mkdir(path, 0700);
}

static int scan_node(const char *src, const char *dst, copy_plan_t *plan) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        perror(src);
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        return add_job(plan, src, dst, &st, JOB_REGULAR);
    }

    if (S_ISLNK(st.st_mode)) {
        return add_job(plan, src, dst, &st, JOB_SYMLINK);
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "copyfast: unsupported file type: %s\n", src);
        errno = ENOTSUP;
        return -1;
    }

    if (ensure_dir(dst) != 0) {
        perror(dst);
        return -1;
    }
    if (add_dir(plan, src, dst, &st) != 0) return -1;

    DIR *dir = opendir(src);
    if (!dir) {
        perror(src);
        return -1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *s = path_join(src, ent->d_name);
        char *d = path_join(dst, ent->d_name);
        if (!s || !d) {
            free(s); free(d); rc = -1; errno = ENOMEM; break;
        }
        if (scan_node(s, d, plan) != 0) rc = -1;
        free(s); free(d);
        if (rc != 0) break;
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    return rc;
}

int build_copy_plan(const char *src, const char *dst, copy_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    return scan_node(src, dst, plan);
}

void free_copy_plan(copy_plan_t *plan) {
    for (size_t i = 0; i < plan->job_count; ++i) {
        free(plan->jobs[i].src);
        free(plan->jobs[i].dst);
    }
    for (size_t i = 0; i < plan->dir_count; ++i) {
        free(plan->dirs[i].src);
        free(plan->dirs[i].dst);
    }
    free(plan->jobs);
    free(plan->dirs);
    memset(plan, 0, sizeof(*plan));
}

int finalize_directories(const copy_plan_t *plan) {
    int rc = 0;
    for (size_t i = plan->dir_count; i > 0; --i) {
        const dir_meta_t *d = &plan->dirs[i-1];
        if (chmod(d->dst, d->st.st_mode & 07777) != 0) {
            perror(d->dst); rc = -1;
        }
        struct timespec ts[2] = { d->st.st_atim, d->st.st_mtim };
        if (utimensat(AT_FDCWD, d->dst, ts, 0) != 0) {
            perror(d->dst); rc = -1;
        }
    }
    return rc;
}

int copy_symlink_job(const copy_job_t *job) {
    size_t cap = job->st.st_size > 0 ? (size_t)job->st.st_size + 1 : 4096;
    char *buf = malloc(cap + 1);
    if (!buf) return -1;
    ssize_t n = readlink(job->src, buf, cap);
    if (n < 0) {
        perror(job->src); free(buf); return -1;
    }
    buf[n] = '\0';

    struct stat dstst;
    if (lstat(job->dst, &dstst) == 0) {
        if (unlink(job->dst) != 0) {
            perror(job->dst); free(buf); return -1;
        }
    } else if (errno != ENOENT) {
        perror(job->dst); free(buf); return -1;
    }

    if (symlink(buf, job->dst) != 0) {
        perror(job->dst); free(buf); return -1;
    }
    free(buf);

    struct timespec ts[2] = { job->st.st_atim, job->st.st_mtim };
    if (utimensat(AT_FDCWD, job->dst, ts, AT_SYMLINK_NOFOLLOW) != 0) {
        perror(job->dst);
        return -1;
    }
    return 0;
}
