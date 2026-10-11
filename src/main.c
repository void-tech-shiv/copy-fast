#include "copyfast.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const copy_plan_t *plan;
    const options_t *opts;
    size_t next;
    pthread_mutex_t lock;
    int errors;
    int cancelled;
} work_state_t;

static void usage(FILE *f) {
    fprintf(f,
        "CopyFast - high-throughput Linux copy/backup tool\n\n"
        "Usage: copyfast [options] SOURCE DESTINATION\n\n"
        "Options:\n"
        "  -b, --backend NAME      threaded (default), naive, io_uring\n"
        "  -j, --jobs N            parallel file jobs (default: 1)\n"
        "  -B, --buffer-size SIZE  per-buffer size, e.g. 1M, 4M (default: 1M)\n"
        "  -q, --queue-depth N     bounded pipeline buffers (default: 4)\n"
        "      --resume            save progress and resume interrupted files\n"
        "      --verify            SHA-256 verification (default)\n"
        "      --no-verify         skip verification (useful for benchmarks)\n"
        "      --quiet             suppress progress display\n"
        "  -h, --help              show this help\n"
        "  -V, --version           show build capabilities\n\n"
        "Examples:\n"
        "  copyfast --resume big.iso backup.iso\n"
        "  copyfast -j 4 Documents Backup/Documents\n"
        "  copyfast --backend naive --no-verify big.bin out.bin\n"
        "  copyfast --backend io_uring --no-verify big.bin out.bin\n");
}

static int parse_size(const char *s, size_t *out) {
    if (!s || !*s) return -1;
    char *end=NULL;
    unsigned long long v=strtoull(s,&end,10);
    if (end==s) return -1;
    unsigned long long mul=1;
    if (*end) {
        if (end[1]) return -1;
        if (*end=='K'||*end=='k') mul=1024ULL;
        else if (*end=='M'||*end=='m') mul=1024ULL*1024ULL;
        else if (*end=='G'||*end=='g') mul=1024ULL*1024ULL*1024ULL;
        else return -1;
    }
    if (v==0 || v > (unsigned long long)SIZE_MAX / mul) return -1;
    *out=(size_t)(v*mul);
    return 0;
}

static int parse_backend(const char *s, backend_t *out) {
    if (strcmp(s,"threaded")==0) *out=BACKEND_THREADED;
    else if (strcmp(s,"naive")==0) *out=BACKEND_NAIVE;
    else if (strcmp(s,"io_uring")==0) *out=BACKEND_IO_URING;
    else return -1;
    return 0;
}

static int run_job(const copy_job_t *j, const options_t *opts) {
    if (j->type == JOB_SYMLINK) return copy_symlink_job(j);
    int rc;
    if (opts->backend == BACKEND_NAIVE) rc = copy_regular_naive(j, opts);
    else if (opts->backend == BACKEND_IO_URING) rc = copy_regular_io_uring(j, opts);
    else rc = copy_regular_threaded(j, opts);
    if (rc < 0) {
        progress_note("ERROR: %s -> %s: %s", j->src, j->dst, strerror(errno));
    }
    return rc;
}

static void *worker_main(void *arg) {
    work_state_t *w=arg;
    for (;;) {
        if (g_cancel_requested) break;
        pthread_mutex_lock(&w->lock);
        if (w->next >= w->plan->job_count) { pthread_mutex_unlock(&w->lock); break; }
        size_t idx=w->next++;
        pthread_mutex_unlock(&w->lock);

        int rc=run_job(&w->plan->jobs[idx],w->opts);
        if (rc != 0) {
            pthread_mutex_lock(&w->lock);
            if (rc > 0) w->cancelled=1; else w->errors++;
            pthread_mutex_unlock(&w->lock);
            if (rc > 0) break;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    options_t opts = {
        .backend=BACKEND_THREADED,
        .buffer_size=1024*1024,
        .queue_depth=4,
        .jobs=1,
        .verify=true,
        .resume=false,
        .quiet=false
    };

    enum { OPT_RESUME=1000, OPT_VERIFY, OPT_NO_VERIFY, OPT_QUIET };
    static const struct option longopts[] = {
        {"backend",required_argument,0,'b'}, {"jobs",required_argument,0,'j'},
        {"buffer-size",required_argument,0,'B'}, {"queue-depth",required_argument,0,'q'},
        {"resume",no_argument,0,OPT_RESUME}, {"verify",no_argument,0,OPT_VERIFY},
        {"no-verify",no_argument,0,OPT_NO_VERIFY}, {"quiet",no_argument,0,OPT_QUIET},
        {"help",no_argument,0,'h'}, {"version",no_argument,0,'V'}, {0,0,0,0}
    };

    int c;
    while ((c=getopt_long(argc,argv,"b:j:B:q:hV",longopts,NULL))!=-1) {
        switch(c) {
            case 'b': if (parse_backend(optarg,&opts.backend)!=0) { fprintf(stderr,"invalid backend\n"); return 2; } break;
            case 'j': { long n=strtol(optarg,NULL,10); if(n<1||n>64){fprintf(stderr,"jobs must be 1..64\n");return 2;} opts.jobs=(unsigned)n; } break;
            case 'B': if(parse_size(optarg,&opts.buffer_size)!=0){fprintf(stderr,"invalid buffer size\n");return 2;} break;
            case 'q': { long n=strtol(optarg,NULL,10); if(n<2||n>128){fprintf(stderr,"queue depth must be 2..128\n");return 2;} opts.queue_depth=(size_t)n; } break;
            case OPT_RESUME: opts.resume=true; break;
            case OPT_VERIFY: opts.verify=true; break;
            case OPT_NO_VERIFY: opts.verify=false; break;
            case OPT_QUIET: opts.quiet=true; break;
            case 'V': printf("CopyFast 1.0\nio_uring: %s\n",io_uring_compiled()?"compiled":"not compiled"); return 0;
            case 'h': usage(stdout); return 0;
            default: usage(stderr); return 2;
        }
    }
    if (argc-optind != 2) { usage(stderr); return 2; }
    if (opts.backend==BACKEND_IO_URING && !io_uring_compiled()) {
        fprintf(stderr,"copyfast: io_uring backend not compiled; install liburing-dev and rebuild.\n");
        return 2;
    }
    if (opts.backend==BACKEND_IO_URING && opts.resume) {
        fprintf(stderr,"copyfast: --resume is supported by threaded/naive backends, not io_uring.\n");
        return 2;
    }
    if (install_signal_handlers()!=0) { perror("sigaction"); return 1; }

    copy_plan_t plan;
    if (build_copy_plan(argv[optind],argv[optind+1],&plan)!=0) {
        free_copy_plan(&plan); return 1;
    }
    progress_init(plan.total_bytes,opts.quiet);
    if (!opts.quiet) {
        progress_note("CopyFast: %zu work item(s), %.2f MiB logical data, backend=%s, jobs=%u",
            plan.job_count,(double)plan.total_bytes/(1024.0*1024.0),
            opts.backend==BACKEND_THREADED?"threaded":opts.backend==BACKEND_NAIVE?"naive":"io_uring",opts.jobs);
    }

    if (opts.jobs > plan.job_count && plan.job_count > 0) opts.jobs=(unsigned)plan.job_count;
    if (plan.job_count == 0) opts.jobs=0;

    work_state_t state={.plan=&plan,.opts=&opts,.next=0,.lock=PTHREAD_MUTEX_INITIALIZER,.errors=0,.cancelled=0};
    pthread_t *threads = opts.jobs ? calloc(opts.jobs,sizeof(*threads)) : NULL;
    if (opts.jobs && !threads) { free_copy_plan(&plan); return 1; }

    unsigned started=0;
    for (unsigned i=0;i<opts.jobs;++i) {
        int e=pthread_create(&threads[i],NULL,worker_main,&state);
        if(e){errno=e;perror("pthread_create");state.errors++;break;} started++;
    }
    for(unsigned i=0;i<started;++i) pthread_join(threads[i],NULL);
    free(threads);

    if (!g_cancel_requested && state.errors==0) {
        if (finalize_directories(&plan)!=0) state.errors++;
    }

    bool ok=!g_cancel_requested && state.errors==0 && !state.cancelled;
    progress_finish(ok);
    free_copy_plan(&plan);
    if (g_cancel_requested || state.cancelled) return 130;
    return state.errors ? 1 : 0;
}
