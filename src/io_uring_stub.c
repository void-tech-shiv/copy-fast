#ifndef HAVE_LIBURING
#include "copyfast.h"
#include <errno.h>

bool io_uring_compiled(void) { return false; }
int copy_regular_io_uring(const copy_job_t *job, const options_t *opts) {
    (void)job; (void)opts;
    errno = ENOTSUP;
    return -1;
}
#endif
