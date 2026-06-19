#ifndef ALLOCATOR_H_
#define ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
  MM_HEAP_STATUS_OK = 0,          // Heap is consistent.
  MM_HEAP_STATUS_NOT_INITIALISED, // mm_init() not yet called.
  MM_HEAP_STATUS_CORRUPT_HEADER,  // Block header metadata invalid.
  MM_HEAP_STATUS_CORRUPT_FOOTER,  // Block footer metadata invalid/mismatched.
  MM_HEAP_STATUS_BAD_BLOCK_SIZE,  // Size/flags inconsistent or not aligned.
  MM_HEAP_STATUS_OUT_OF_BOUNDS,   // Block extends outside managed heap.
  MM_HEAP_STATUS_INPROG_BLOCK,    // A block is left in INPROG state.
  MM_HEAP_STATUS_ADJACENT_FREE,   // Two neighbouring free blocks (missed coalesce).
  MM_HEAP_STATUS_PATTERN_MISMATCH // Free block payload not filled with pattern.
} mm_heap_status_t;

#define MM_ALIGN 40

// Initializes allocator to manage the given heap buffer.
// Returns 0 on success, non-zero on error.
int mm_init(uint8_t* heap, size_t heap_size);

// Allocates a block with at least |size| usable bytes.
// Returns pointer to payload, or NULL on failure/corruption.
void* mm_malloc(size_t size);

// Allocates an array of nmemb elements of size |size|, zero-initialised.
// Returns pointer to payload, or NULL on failure/overflow.
void* mm_calloc(size_t nmemb, size_t size);

// Reads |len| bytes from block starting at |offset| into |buf|.
// Returns number of bytes read, or -1 on error.
int mm_read(void* ptr, size_t offset, void* buf, size_t len);

// Writes |len| bytes from |src| into block starting at |offset|.
// Returns number of bytes written, or -1 on error.
int mm_write(void* ptr, size_t offset, const void* src, size_t len);

// Frees a previously allocated block.
// Invalid/double frees are quarantined when possible.
void mm_free(void* ptr);

// Optional helpers for testing/extra credit.
void* mm_realloc(void* ptr, size_t new_size);
void mm_heap_stats(void);

// Validates the entire heap and returns the first detected error code.
mm_heap_status_t mm_heap_validate(void);

#endif  // ALLOCATOR_H_
