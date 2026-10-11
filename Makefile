CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Iinclude
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS += -pthread

HAS_LIBURING := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing && echo 1 || echo 0)

COMMON_SRC := src/main.c src/fs.c src/copy.c src/util.c src/sparse.c src/sha256.c
ifeq ($(HAS_LIBURING),1)
  CFLAGS += $(shell pkg-config --cflags liburing) -DHAVE_LIBURING
  LDLIBS += $(shell pkg-config --libs liburing)
  URING_SRC := src/io_uring_copy.c
else
  URING_SRC := src/io_uring_stub.c
endif

SRC := $(COMMON_SRC) $(URING_SRC)
OBJ := $(SRC:.c=.o)
TARGET := copyfast

.PHONY: all clean test format info

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c include/copyfast.h
	$(CC) $(CFLAGS) -c $< -o $@

info:
	@echo "liburing detected: $(HAS_LIBURING)"
	@echo "compiler: $(CC)"

clean:
	rm -f $(OBJ) src/io_uring_copy.o src/io_uring_stub.o $(TARGET)

test: $(TARGET)
	bash tests/run_all.sh

format:
	clang-format -i include/*.h src/*.c
