# CopyFast design

## Goal

CopyFast is a Linux/C11 copy and backup utility designed to demonstrate operating-system and system-programming concepts: system calls, file descriptors, sparse files, concurrency, bounded-buffer producer/consumer synchronization, signals, metadata, verification, resumability, and optional `io_uring`.

## Default overlap model

For each regular file the default backend creates one reader and one writer thread. They communicate through a fixed-size ring of preallocated buffers. Two POSIX semaphores implement backpressure:

- `empty`: number of buffers the reader may fill.
- `full`: number of buffers the writer may drain.

The reader uses `pread()` and the writer uses `pwrite()`. Because each side operates on separate buffers, device reads and writes can overlap. The queue depth and buffer size are tunable from the CLI.

## Sparse files

The reader walks source extents using `lseek(SEEK_DATA)` and `lseek(SEEK_HOLE)`. Only data extents are enqueued. The writer writes each chunk at its logical offset using `pwrite()`, leaving holes untouched. At completion, `ftruncate()` sets the exact logical length, including a trailing hole.

If the source filesystem does not support `SEEK_DATA/SEEK_HOLE`, CopyFast falls back to a dense transfer for compatibility.

## Resume

With `--resume`, each destination has a small sidecar file named `<destination>.copyfast.resume`. It stores source size, source mtime, and the highest contiguous logical offset durably written by the FIFO writer. Updates are written to a temporary sidecar, `fsync()`ed, and atomically renamed.

On the next run, the sidecar is accepted only when the source size/mtime still match and the destination is at least as large as the recorded offset. Otherwise CopyFast restarts that file from zero. On a successful verified transfer, the sidecar is removed.

The `io_uring` demonstration backend intentionally rejects `--resume`; the fully resumable backend is the default threaded engine.

## Signals

The SIGINT/SIGTERM handler performs only the async-signal-safe action of setting a `sig_atomic_t` flag. The reader notices the flag, stops producing new chunks, and places a cancellation marker into the bounded queue. The writer drains already queued data, records progress when resume is enabled, closes cleanly, and the process exits with status 130.

## Metadata

Regular-file mode and atime/mtime are restored after data transfer using `fchmod()` and `futimens()`. Directory metadata is applied in reverse traversal order after all children finish so creating/copying children does not destroy the final directory timestamps. Symlink timestamps are preserved with `utimensat(..., AT_SYMLINK_NOFOLLOW)`.

Ownership is deliberately not changed because `chown` commonly requires privileges and the assignment specifically requires mode/timestamps.

## Verification

SHA-256 is implemented in-tree, so CopyFast has no crypto-library dependency. The default behavior hashes source and destination after each copied regular file and fails the job if the digests differ. `--no-verify` exists for controlled throughput benchmarks where verification would measure hashing as well as copying.

## Parallel multi-file copy

`--jobs N` creates a work pool for independent file/symlink jobs. Each regular-file job still uses the bounded reader/writer pipeline, so backpressure exists at the per-file level. This demonstrates parallel speedup while avoiding an unbounded memory queue.

Approximate maximum data-buffer memory is:

`jobs × queue_depth × buffer_size`

For example `-j 4 -q 4 -B 1M` uses about 16 MiB of pipeline data buffers plus overhead.

## io_uring backend

When `liburing` is available at build time, CopyFast compiles an `io_uring` backend. It keeps multiple read/write operations in flight and reuses a fixed pool of buffers. Sparse extents are submitted with explicit offsets, so holes remain sparse. This backend is selected with `--backend io_uring` and is intended for comparison with the pthread pipeline.

## Benchmark methodology

`benchmark/benchmark.sh` compares `naive`, `threaded`, and (when compiled) `io_uring` with verification disabled. The naive backend is deliberately sequential: blocking `read()` followed by blocking `write()`.

Page cache and storage hardware strongly affect results. For a report, use the same source/destination device setup for all backends, run several rounds, and report the median. A threaded design may not beat naive copying on every machine (especially when both source and destination are the same fast SSD and data is cache-resident); the experiment should report measured results rather than fabricated numbers.

## Acceptance mapping

- Byte-identical copy: default SHA-256 verification plus integrity tests.
- Metadata: mode and timestamps preserved and tested.
- Sparse preservation: SEEK_DATA/SEEK_HOLE + pwrite offsets + `test_sparse.sh`.
- Resume: sidecar checkpoints + SIGINT test.
- Overlap/backpressure: pthread producer/consumer + bounded semaphore queue.
- Throughput comparison: naive vs threaded benchmark script.
- Distinction features: optional io_uring backend and `--jobs N` parallel multi-file work pool.
