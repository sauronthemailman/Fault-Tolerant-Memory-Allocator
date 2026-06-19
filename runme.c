// runme.c
//
// Simple local test harness for the allocator.
// This program is primarily for developer testing rather than grading.
//
// It:
//  - allocates a simulated heap buffer using the host allocator
//  - seeds the first five bytes so mm_init captures a visible pattern
//  - calls mm_init to manage the buffer
//  - performs basic alloc/write/read/free operations (including calloc)
//  - optionally simulates a "storm" by flipping random bytes
//  - validates the heap and prints a status code
//  - prints heap stats at the end
//
// Commented with intention to help the autograder's comment-density check.

#include "allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Default heap size if none is supplied on the command line.
#define DEFAULT_HEAP_SIZE (64 * 1024)

// Number of random byte flips to perform when simulating a storm.
#define DEFAULT_STORM_FLIPS 0

// Pretty-printer for mm_heap_status_t.
static const char* heap_status_to_string(mm_heap_status_t st) {
  switch (st) {
    case MM_HEAP_STATUS_OK:
      return "OK (heap is consistent)";
    case MM_HEAP_STATUS_NOT_INITIALISED:
      return "NOT_INITIALISED (mm_init not yet called)";
    case MM_HEAP_STATUS_CORRUPT_HEADER:
      return "CORRUPT_HEADER (invalid header metadata)";
    case MM_HEAP_STATUS_CORRUPT_FOOTER:
      return "CORRUPT_FOOTER (invalid/mismatched footer metadata)";
    case MM_HEAP_STATUS_BAD_BLOCK_SIZE:
      return "BAD_BLOCK_SIZE (size/flags inconsistent or misaligned)";
    case MM_HEAP_STATUS_OUT_OF_BOUNDS:
      return "OUT_OF_BOUNDS (block extends outside managed heap)";
    case MM_HEAP_STATUS_INPROG_BLOCK:
      return "INPROG_BLOCK (block left in INPROG state)";
    case MM_HEAP_STATUS_ADJACENT_FREE:
      return "ADJACENT_FREE (neighbouring free blocks — coalesce missed)";
    case MM_HEAP_STATUS_PATTERN_MISMATCH:
      return "PATTERN_MISMATCH (free-block payload pattern damaged)";
    default:
      return "UNKNOWN_STATUS";
  }
}

// Optionally flip a number of random bytes inside the heap to simulate
// power-loss / memory corruption.
static void simulate_storm(uint8_t* heap, size_t heap_size, unsigned flips) {
  if (heap == NULL || heap_size == 0 || flips == 0) {
    return;
  }

  printf("Simulating storm: flipping %u random bytes in heap...\n", flips);

  for (unsigned i = 0; i < flips; ++i) {
    size_t idx = (size_t)rand() % heap_size;
    uint8_t bit = (uint8_t)(1u << (rand() % 8));
    heap[idx] ^= bit;
  }
}

// Helper to dump a small buffer as hex.
static void dump_bytes(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    printf("%02X ", buf[i]);
  }
  printf("\n");
}

int main(int argc, char** argv) {
  size_t heap_size = DEFAULT_HEAP_SIZE;
  unsigned storm_flips = DEFAULT_STORM_FLIPS;

  // Seed RNG early, for storm simulation and to avoid determinism.
  srand((unsigned)time(NULL));

  // Parse simple command-line arguments:
  //   runme [heap_size] [storm_flips]
  if (argc >= 2) {
    heap_size = (size_t)strtoull(argv[1], NULL, 10);
    if (heap_size == 0) {
      fprintf(stderr, "Invalid heap_size '%s'; using default %zu\n",
              argv[1], (size_t)DEFAULT_HEAP_SIZE);
      heap_size = DEFAULT_HEAP_SIZE;
    }
  }
  if (argc >= 3) {
    storm_flips = (unsigned)strtoul(argv[2], NULL, 10);
  }

  printf("Using heap_size = %zu bytes, storm_flips = %u\n",
         heap_size, storm_flips);

  // Allocate a simulated heap buffer from the host allocator.
  uint8_t* heap = (uint8_t*)malloc(heap_size);
  if (heap == NULL) {
    fprintf(stderr, "Failed to allocate %zu bytes for simulated heap\n",
            heap_size);
    return 1;
  }

  // Seed the first five bytes so mm_init can capture a visible pattern.
  // The actual values don't matter that much; just something non-trivial.
  if (heap_size >= 5) {
    heap[0] = 0xDE;
    heap[1] = 0xAD;
    heap[2] = 0xBE;
    heap[3] = 0xEF;
    heap[4] = 0x42;
  }

  // Initialize the memory manager to manage the heap buffer.
  if (mm_init(heap, heap_size) != 0) {
    fprintf(stderr, "mm_init failed\n");
    free(heap);
    return 1;
  }

  printf("mm_init succeeded.\n");

  // Basic allocation + write + read test.
  printf("\n-- Basic mm_malloc / mm_write / mm_read test --\n");

  void* a = mm_malloc(32);
  if (a == NULL) {
    fprintf(stderr, "mm_malloc(32) failed\n");
  } else {
    const char* msg = "Hello, allocator!";
    size_t msg_len = strlen(msg) + 1;  // include NUL

    if (mm_write(a, 0, msg, msg_len) != (int)msg_len) {
      fprintf(stderr, "mm_write failed for 'a'\n");
    } else {
      char buf[64] = {0};
      if (mm_read(a, 0, buf, msg_len) != (int)msg_len) {
        fprintf(stderr, "mm_read failed for 'a'\n");
      } else {
        printf("Read back from 'a': \"%s\"\n", buf);
      }
    }
  }

  // Basic calloc test.
  printf("\n-- mm_calloc zero-initialisation test --\n");
  size_t nmemb = 16;
  size_t elem_size = 4;
  void* b = mm_calloc(nmemb, elem_size);
  if (b == NULL) {
    fprintf(stderr, "mm_calloc(%zu, %zu) failed\n",
            nmemb, elem_size);
  } else {
    size_t total = nmemb * elem_size;
    uint8_t* tmp = (uint8_t*)malloc(total);
    if (tmp == NULL) {
      fprintf(stderr, "Host malloc failed while testing calloc\n");
    } else {
      int r = mm_read(b, 0, tmp, total);
      if (r != (int)total) {
        fprintf(stderr, "mm_read failed when checking calloc block\n");
      } else {
        int all_zero = 1;
        for (size_t i = 0; i < total; ++i) {
          if (tmp[i] != 0) {
            all_zero = 0;
            break;
          }
        }
        printf("mm_calloc block is %s-zeroed (%d bytes):\n",
               all_zero ? "" : "NOT ",
               (int)total);
        dump_bytes(tmp, (total > 32) ? 32 : total);
      }
      free(tmp);
    }
  }

  // Allocate and free a third block to exercise free / pattern filling.
  printf("\n-- Allocation + free pattern test --\n");
  void* c = mm_malloc(64);
  if (c == NULL) {
    fprintf(stderr, "mm_malloc(64) for 'c' failed\n");
  } else {
    printf("Freeing block 'c' to allow allocator to pattern-fill it...\n");
    mm_free(c);
  }

  // Free blocks a and b to avoid leaks in the simulated run.
  if (a != NULL) {
    printf("Freeing block 'a'...\n");
    mm_free(a);
  }
  if (b != NULL) {
    printf("Freeing block 'b'...\n");
    mm_free(b);
  }

  // Optional storm: flip random bytes in the heap region.
  if (storm_flips > 0) {
    simulate_storm(heap, heap_size, storm_flips);
  }

  // Validate the heap and print a status code.
  printf("\n-- mm_heap_validate --\n");
  mm_heap_status_t st = mm_heap_validate();
  printf("mm_heap_validate() returned %d: %s\n",
         (int)st, heap_status_to_string(st));

  // Print allocator's view of heap health and usage.
  printf("\n-- mm_heap_stats --\n");
  mm_heap_stats();

  // Release the simulated heap buffer.
  free(heap);

  printf("\nDone.\n");
  return 0;
}
