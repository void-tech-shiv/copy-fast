#include "copyfast.h"

#include <errno.h>
#include <unistd.h>

void extent_iter_init(extent_iter_t *it, int fd, off_t size, off_t start) {
    it->fd = fd;
    it->size = size;
    it->pos = start;
    it->extent_end = start;
    it->dense_fallback = false;
    it->have_extent = false;
}

int extent_next(extent_iter_t *it, size_t max_len, off_t *offset, size_t *length) {
    if (it->pos >= it->size) {
        return 0;
    }

    for (;;) {
        if (it->dense_fallback) {
            off_t remain = it->size - it->pos;
            size_t n = remain < (off_t)max_len ? (size_t)remain : max_len;
            *offset = it->pos;
            *length = n;
            it->pos += (off_t)n;
            return 1;
        }

        if (!it->have_extent || it->pos >= it->extent_end) {
            errno = 0;
            off_t data = lseek(it->fd, it->pos, SEEK_DATA);
            if (data == (off_t)-1) {
                if (errno == ENXIO) {
                    return 0; /* trailing hole or fully sparse file */
                }
                if (errno == EINVAL || errno == ENOTSUP) {
                    it->dense_fallback = true;
                    continue;
                }
                return -1;
            }
            if (data >= it->size) {
                return 0;
            }

            errno = 0;
            off_t hole = lseek(it->fd, data, SEEK_HOLE);
            if (hole == (off_t)-1) {
                if (errno == ENXIO) {
                    hole = it->size;
                } else if (errno == EINVAL || errno == ENOTSUP) {
                    it->dense_fallback = true;
                    continue;
                } else {
                    return -1;
                }
            }
            if (hole > it->size) {
                hole = it->size;
            }

            it->pos = data;
            it->extent_end = hole;
            it->have_extent = true;
            if (it->pos >= it->extent_end) {
                it->have_extent = false;
                continue;
            }
        }

        off_t remain = it->extent_end - it->pos;
        size_t n = remain < (off_t)max_len ? (size_t)remain : max_len;
        *offset = it->pos;
        *length = n;
        it->pos += (off_t)n;
        return 1;
    }
}
