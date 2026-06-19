# Fault-Tolerant Memory Allocator

A dynamic memory allocator for a **single contiguous heap**, written in C and designed to keep working through **radiation-induced bit flips** and **brownout (partial-write) power events** — the kind of faults an embedded system on, say, a Mars rover has to tolerate without a reset.

It manages only the buffer you hand it, stores all of its bookkeeping *inside* that buffer using redundant boundary tags, and can detect and quarantine corruption rather than crashing or returning poisoned memory.

---

## Why

Standard allocators assume RAM is reliable. In high-radiation or unstable-power environments that assumption breaks: a single flipped bit in a size field or free-list pointer can corrupt the whole heap. This allocator is built around the opposite assumption — that metadata *will* get corrupted — and is engineered to notice and recover.

---

## Design

**40-byte aligned blocks.** Every block is aligned to a fixed `MM_ALIGN = 40` boundary relative to the original heap base. Because the stride is fixed, the validator can resynchronise to the next plausible block even after corruption instead of blindly trusting a damaged size field.

**Mirrored header/footer metadata.** Each block carries a `BlockHeader` and `BlockFooter`, and each of those holds two copies of a `MetaUnit` (a primary and a mirror). A `MetaUnit` stores:

- a `magic` value **and** its bitwise inverse (`magic_inv`)
- `size_and_flags` **and** its bitwise inverse (`size_inv`)
- a checksum

Storing inverted copies means a bit flip in a field no longer matches its complement, so single-bit corruption is detectable rather than silently valid.

**FNV-1a-style checksum over metadata *and* payload.** The checksum doesn't just cover the header — it mixes in the payload bytes too, so corruption of user data is also caught on validation.

**Detectable free-space pattern.** The first 5 bytes of the heap are captured at `mm_init` and used as a repeating pattern to fill free regions. The pattern's phase is anchored to the original heap base, so a free block whose payload no longer matches the expected pattern flags a `PATTERN_MISMATCH`.

**Radiation resilience.** Headers are validated independently, the validator never trusts a size from a corrupt header, and it resyncs in fixed `MM_ALIGN` steps to recover the block stream.

**Brownout resilience.** Mutating operations stage writes behind an explicit `FLAG_INPROG` marker: the mirror is written first, the primary is committed second. A block left in the `INPROG` state after a power loss is detectable, so a half-finished write doesn't masquerade as a valid block.

**Corruption quarantine.** Blocks that fail validation (and invalid/double frees) are marked `FLAG_BAD` and quarantined where possible, rather than being recycled into the free pool.

---

## API

```c
#include "allocator.h"

int   mm_init(uint8_t* heap, size_t heap_size);          // hand the allocator a buffer to manage
void* mm_malloc(size_t size);                            // allocate >= size usable bytes
void* mm_calloc(size_t nmemb, size_t size);              // zero-initialised array allocation
int   mm_read(void* ptr, size_t offset, void* buf, size_t len);        // validated read
int   mm_write(void* ptr, size_t offset, const void* src, size_t len); // validated write
void  mm_free(void* ptr);                                // invalid/double frees are quarantined
void* mm_realloc(void* ptr, size_t new_size);
void  mm_heap_stats(void);
mm_heap_status_t mm_heap_validate(void);                 // full-heap check, returns first error
```

`mm_read` / `mm_write` go through the metadata checks, so reads and writes against a corrupted block fail (`-1`) instead of touching bad memory.

### Heap status codes

`mm_heap_validate()` returns the first problem it finds:

| Status | Meaning |
| --- | --- |
| `MM_HEAP_STATUS_OK` | Heap is consistent |
| `MM_HEAP_STATUS_NOT_INITIALISED` | `mm_init()` not called yet |
| `MM_HEAP_STATUS_CORRUPT_HEADER` | Block header metadata invalid |
| `MM_HEAP_STATUS_CORRUPT_FOOTER` | Block footer metadata invalid/mismatched |
| `MM_HEAP_STATUS_BAD_BLOCK_SIZE` | Size/flags inconsistent or not aligned |
| `MM_HEAP_STATUS_OUT_OF_BOUNDS` | Block extends outside the managed heap |
| `MM_HEAP_STATUS_INPROG_BLOCK` | A block was left mid-write (brownout) |
| `MM_HEAP_STATUS_ADJACENT_FREE` | Two neighbouring free blocks (missed coalesce) |
| `MM_HEAP_STATUS_PATTERN_MISMATCH` | Free-block payload pattern damaged |

---

## Building & running

The allocator is a single translation unit (`allocator.c` + `allocator.h`).

**Quick test harness** — allocates a simulated heap, runs alloc/write/read/free, optionally injects faults, then validates:

```bash
gcc -O2 -Wall -o runme runme.c allocator.c
./runme [heap_size] [storm_flips]
# e.g. flip 50 random bytes in a 64 KiB heap, then validate:
./runme 65536 50
```

**Benchmark suite** — expects the allocator as a shared library:

```bash
gcc -shared -fPIC -O2 -o liballocator.so allocator.c
gcc -O2 -o full_benchmark full_benchmark.c -L. -lallocator -Wl,-rpath,'$ORIGIN'
./full_benchmark
```

Optional thread safety: compile with `-DMM_THREAD_SAFE` (and link `-lpthread`) to guard the allocator with a mutex.

---

## Benchmarks

`full_benchmark.c` runs four tests under a 1 MB embedded-style heap constraint with a high-precision monotonic timer and radiation/brownout fault injection:

1. **Payload efficiency** — usable bytes vs total heap consumed
2. **Allocation vs deallocation latency**
3. **Read/write throughput** vs a raw `memcpy` hardware baseline
4. **Sustained "Mars mission" stress test** (~1,000,000 operations)

Representative results:

- Up to **95.7% payload efficiency**
- **Constant-time frees** in single-digit microseconds, versus linear-scan allocation costing up to **~1.7 ms**
- Read/write throughput profiled against a `memcpy` baseline

---

## Files

| File | Purpose |
| --- | --- |
| `allocator.h` | Public API and status codes |
| `allocator.c` | Allocator implementation |
| `runme.c` | Developer test harness with optional fault injection |
| `full_benchmark.c` | Comprehensive benchmark suite |

---

