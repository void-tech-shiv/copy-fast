# CopyFast

**CopyFast** is a high-throughput Linux file copy/backup tool written in C11 for an Operating Systems & System Programming project.

It demonstrates:

- recursive file/directory copying
- mode + timestamp preservation
- overlapped reader/writer I/O using pthreads
- bounded-buffer producer/consumer backpressure
- sparse-file preservation with `SEEK_DATA` / `SEEK_HOLE`
- resumable transfers after `SIGINT`
- built-in SHA-256 verification
- progress + throughput reporting
- naive blocking baseline for benchmarks
- parallel multi-file copying with `--jobs`
- optional `io_uring` backend when `liburing` is installed

## Architecture

```text
                      +-------------------+
Source file --------> |   Reader thread   |
                      +---------+---------+
                                |
                        preallocated ring
                       +----+----+----+----+
                       | B0 | B1 | B2 | B3 |   bounded queue
                       +----+----+----+----+
                                |
                      +---------v---------+
Destination <---------|   Writer thread   |
                      +-------------------+

empty/full semaphores provide backpressure.
Sparse holes never enter the queue; data chunks carry logical offsets.
```

## Build on Ubuntu

```bash
sudo apt update
sudo apt install -y build-essential pkg-config
make
```

For the optional distinction-level `io_uring` backend:

```bash
sudo apt install -y liburing-dev
make clean && make
./copyfast --version
```

Expected capability output:

```text
CopyFast 1.0
io_uring: compiled
```

## Usage

```bash
./copyfast [options] SOURCE DESTINATION
```

Examples:

```bash
# Verified threaded copy (default)
./copyfast ubuntu.iso backup/ubuntu.iso

# Recursive directory backup, four files at a time
./copyfast -j 4 ~/Documents ~/Backup/Documents

# Resumable copy
./copyfast --resume huge.img backup/huge.img

# Tune the producer/consumer pipeline
./copyfast -B 4M -q 8 source.bin destination.bin

# Sequential baseline
./copyfast --backend naive --no-verify source.bin naive.bin

# io_uring backend (when compiled)
./copyfast --backend io_uring --no-verify source.bin uring.bin
```

Run `./copyfast --help` for all options.

## Resume demo

Start a large transfer:

```bash
./copyfast --resume big-file.iso backup.iso
```

Press `Ctrl+C`. CopyFast drains already queued writes, leaves `backup.iso.copyfast.resume`, and exits with code 130. Run the same command again:

```bash
./copyfast --resume big-file.iso backup.iso
```

It validates the checkpoint against source size/mtime and continues from the recorded offset instead of restarting.

## Sparse-file demo

```bash
truncate -s 1G sparse.img
echo hello | dd of=sparse.img bs=1 seek=536870912 conv=notrunc status=none
./copyfast sparse.img sparse-copy.img
stat -c 'logical=%s bytes blocks=%b' sparse.img sparse-copy.img
cmp sparse.img sparse-copy.img
```

The destination remains sparse instead of physically writing the hole as zero-filled blocks.

## Test suite

```bash
make test
```

Tests cover:

- normal file integrity + built-in verification
- recursive directories + symlinks
- sparse preservation
- mode + mtime preservation
- resume after SIGINT
- naive baseline correctness

## Benchmark

```bash
./benchmark/benchmark.sh 512
```

The argument is the test size in MiB. The script compares sequential `read`/`write`, threaded overlap, and `io_uring` when available. Verification is disabled during the benchmark so the measurement focuses on transfer throughput.

**Important:** storage hardware, page cache, filesystem and source/destination placement affect results. Run several rounds and report the median; do not fabricate a speedup if a particular machine does not show one.

## Key Linux APIs used

`open`, `read`, `write`, `pread`, `pwrite`, `lseek(SEEK_DATA/SEEK_HOLE)`, `fstat`, `ftruncate`, `fchmod`, `futimens`, `utimensat`, `posix_fadvise`, `pthread_create`, POSIX semaphores, `sigaction`, and optionally `io_uring` via `liburing`.

## Project layout

```text
include/copyfast.h          shared declarations
src/main.c                  CLI + parallel work queue
src/fs.c                    recursive traversal + symlinks + directory metadata
src/copy.c                  naive + pthread pipeline + resume integration
src/sparse.c                sparse extent iterator
src/sha256.c                dependency-free SHA-256
src/util.c                  progress, signals, metadata, resume helpers
src/io_uring_copy.c         optional async backend
src/io_uring_stub.c         fallback when liburing is absent
tests/                      acceptance tests
benchmark/benchmark.sh      throughput comparison
docs/design.md              design/viva document
```

## Viva summary

> CopyFast improves copy throughput by overlapping source reads and destination writes through a bounded producer/consumer buffer pipeline. It preserves sparse holes using SEEK_DATA/SEEK_HOLE and offset writes, preserves mode/timestamps, can checkpoint and resume interrupted transfers, and verifies byte identity with SHA-256. A naive sequential backend provides the benchmark baseline, while optional io_uring and parallel file jobs demonstrate modern asynchronous and parallel I/O.

## Exit codes

- `0` success
- `1` copy/verification/runtime failure
- `2` command-line/configuration error
- `130` interrupted by SIGINT/SIGTERM

## License

MIT
