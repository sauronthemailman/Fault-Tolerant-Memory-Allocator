// allocator.c
//
// Robust allocator for a single contiguous heap region.
//
// Core goals:
//  - Manage only the heap buffer provided by mm_init.
//  - Use header/footer metadata stored inside this region.
//  - Maintain a detectable free-space pattern.
//  - Survive radiation storms and brownout events at a minimal level.
//
// Key techniques:
//  - 40-byte alignment relative to the original heap base.
//  - Header/footer boundary tags with mirrored MetaUnits.
//  - MetaUnits contain magic, inverted fields, and checksums.
//  - 5-byte unused pattern captured from first 5 bytes of the heap.
//  - Pattern phase stays aligned to the original base across frees.
//  - Radiation resilience:
//      * validate headers independently AND PAYLOADS (FNV-1a Checksum)
//      * resync in fixed MM_ALIGN steps
//      * never trust size fields from corrupt headers
//  - Brownout resilience:
//      * explicit FLAG_INPROG marker
//      * staged mirror writes, committed primary writes

#include "allocator.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef MM_THREAD_SAFE
#include <pthread.h>
static pthread_mutex_t g_mm_lock = PTHREAD_MUTEX_INITIALIZER;
static void mm_lock(void) { pthread_mutex_lock(&g_mm_lock); }
static void mm_unlock(void) { pthread_mutex_unlock(&g_mm_lock); }
#else
static void mm_lock(void) {}
static void mm_unlock(void) {}
#endif

// -----------------------------------------------------------------------------
// Global heap state.
// -----------------------------------------------------------------------------
static uint8_t* g_heap = NULL;       // Managed heap start (after offset).
static size_t g_heap_size = 0;       // Managed heap size.

static uint8_t* g_heap_base = NULL;  // Original pointer passed to mm_init.
static size_t g_heap_total = 0;      // Original size passed to mm_init.

// -----------------------------------------------------------------------------
// 5-byte pattern derived from the first 5 bytes of the heap.
// -----------------------------------------------------------------------------
static uint8_t g_unused_pattern[5] = {0, 0, 0, 0, 0};
static bool g_pattern_captured = false;

static void capture_unused_pattern(const uint8_t* heap) {
  // Store the first five bytes of the heap as the pattern source.
  for (size_t i = 0; i < 5; i++) {
    g_unused_pattern[i] = heap[i];
  }
  g_pattern_captured = true;
}

static uint8_t pattern_byte(size_t index) {
  if (!g_pattern_captured) return 0;
  return g_unused_pattern[index % 5];
}

// Fill a region with the 5-byte repeating pattern.
// The pattern phase is aligned to the original heap base pointer.
static void fill_pattern_aligned(uint8_t* p, size_t len) {
  if (p == NULL || len == 0) return;

  // The start position in the 5-byte cycle depends on distance
  // from the original heap base, not the managed heap start.
  size_t start = 0;
  if (g_heap_base != NULL) {
    start = (size_t)(p - g_heap_base) % 5;
  }

  for (size_t i = 0; i < len; i++) {
    p[i] = pattern_byte(start + i);
  }
}

// Check whether a region matches the expected aligned pattern.
// This can be used as a corruption hint.
static bool looks_like_pattern_aligned(const uint8_t* p, size_t len) {
  if (p == NULL || len == 0) return true;

  size_t start = 0;
  if (g_heap_base != NULL) {
    start = (size_t)(p - g_heap_base) % 5;
  }

  for (size_t i = 0; i < len; i++) {
    if (p[i] != pattern_byte(start + i)) return false;
  }

  return true;
}

// Non-fatal init-time pattern sample check.
// This does not influence allocator state; it's just a detection hint.
static bool detect_heap_pattern_sample(uint8_t* heap, size_t heap_size) {
  if (heap == NULL || heap_size == 0) return false;
  size_t n = (heap_size < 64) ? heap_size : 64;
  return looks_like_pattern_aligned(heap, n);
}

// -----------------------------------------------------------------------------
// Flags and alignment.
// -----------------------------------------------------------------------------
#define FLAG_ALLOC  ((size_t)1u)
#define FLAG_BAD    ((size_t)2u)
#define FLAG_INPROG ((size_t)4u)
#define MM_ALIGN 40

static size_t align_up(size_t n, size_t a) {
  size_t r = n % a;
  return (r == 0) ? n : (n + (a - r));
}

static size_t align_down(size_t n, size_t a) {
  return n - (n % a);
}

// -----------------------------------------------------------------------------
// Mirrored metadata.
// -----------------------------------------------------------------------------
#define MAGIC_HEAD 0xC0FFEE11u
#define MAGIC_FOOT 0xC0FFEE22u

typedef struct {
  uint32_t magic;
  uint32_t magic_inv;
  size_t size_and_flags;
  size_t size_inv;
  uint32_t checksum;
} MetaUnit;

typedef struct {
  MetaUnit primary;
  MetaUnit mirror;
} BlockHeader;

typedef struct {
  MetaUnit primary;
  MetaUnit mirror;
} BlockFooter;

static size_t safe_size(size_t sf) {
  return sf & ~(FLAG_ALLOC | FLAG_BAD | FLAG_INPROG);
}
static bool safe_alloc(size_t sf)   { return (sf & FLAG_ALLOC)  != 0; }
static bool safe_bad(size_t sf)     { return (sf & FLAG_BAD)    != 0; }
static bool safe_inprog(size_t sf)  { return (sf & FLAG_INPROG) != 0; }

// A strong checksum using FNV-1a-like mixing that covers metadata AND payload.
static uint32_t checksum_meta(uint32_t magic, size_t size_and_flags,
                              const uint8_t* payload, size_t len) {
  // Golden Ratio constant (phi) as initial seed
  uint64_t hash = 0x9E3779B97F4A7C15ULL;
  // Robust LCG multiplier (from musl libc)
  uint64_t prime = 6364136223846793005ULL;

  hash = (hash ^ magic) * prime;
  hash = (hash ^ size_and_flags) * prime;

  // If the block is allocated and STABLE (not in-progress), hash the payload.
  // This detects radiation damage to the user's data.
  if (safe_alloc(size_and_flags) && !safe_inprog(size_and_flags) &&
      payload != NULL) {
    for (size_t i = 0; i < len; i++) {
      hash = (hash ^ payload[i]) * prime;
    }
  }

  return (uint32_t)(hash ^ (hash >> 32));
}

// Populate a MetaUnit consistently.
static void meta_unit_write(MetaUnit* u, uint32_t magic,
                            size_t size_and_flags,
                            const uint8_t* payload, size_t len) {
  u->magic = magic;
  u->magic_inv = ~magic;
  u->size_and_flags = size_and_flags;
  u->size_inv = ~size_and_flags;
  u->checksum = checksum_meta(magic, size_and_flags, payload, len);
}

// Validate a MetaUnit without referencing any other structures.
static bool meta_unit_valid(const MetaUnit* u, uint32_t expected_magic,
                            const uint8_t* payload, size_t len) {
  if (u->magic != expected_magic) return false;
  if (u->magic_inv != ~expected_magic) return false;
  if (u->size_inv != ~u->size_and_flags) return false;
  if (u->checksum !=
      checksum_meta(u->magic, u->size_and_flags, payload, len)) {
    return false;
  }
  return true;
}

// Pick the best surviving meta between primary and mirror.
// Rejects any entry with FLAG_INPROG set.
static bool meta_pick(const MetaUnit* primary, const MetaUnit* mirror,
                      uint32_t magic, size_t* out_sf,
                      const uint8_t* payload, size_t len) {
  bool p_valid = meta_unit_valid(primary, magic, payload, len);
  bool m_valid = meta_unit_valid(mirror, magic, payload, len);

  // Reject in-progress updates.
  if (p_valid && (primary->size_and_flags & FLAG_INPROG)) p_valid = false;
  if (m_valid && (mirror->size_and_flags & FLAG_INPROG)) m_valid = false;

  if (p_valid && m_valid) {
    *out_sf = primary->size_and_flags;
    return true;
  }
  if (m_valid) {
    *out_sf = mirror->size_and_flags;
    return true;
  }
  if (p_valid) {
    *out_sf = primary->size_and_flags;
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Bounds checks.
// -----------------------------------------------------------------------------
static bool header_in_heap(const BlockHeader* h) {
  const uint8_t* p = (const uint8_t*)h;
  return (g_heap != NULL && p >= g_heap &&
          p + sizeof(*h) <= g_heap + g_heap_size);
}

static bool footer_in_heap(const BlockFooter* f) {
  const uint8_t* p = (const uint8_t*)f;
  return (g_heap != NULL && p >= g_heap &&
          p + sizeof(*f) <= g_heap + g_heap_size);
}

// -----------------------------------------------------------------------------
// Block helpers.
// -----------------------------------------------------------------------------
static size_t min_block_size(void) {
  // Minimum legal block must hold: header + footer + 1 payload byte
  size_t m = sizeof(BlockHeader) + sizeof(BlockFooter) + 1;
  return align_up(m, MM_ALIGN);
}

static uint8_t* payload_ptr(BlockHeader* h) {
  return (uint8_t*)h + sizeof(BlockHeader);
}

static size_t payload_size_from_block(size_t blk_size) {
  if (blk_size < sizeof(BlockHeader) + sizeof(BlockFooter)) return 0;
  return blk_size - sizeof(BlockHeader) - sizeof(BlockFooter);
}

static BlockFooter* footer_by_size(BlockHeader* h, size_t blk_size) {
  return (BlockFooter*)((uint8_t*)h + blk_size - sizeof(BlockFooter));
}

static BlockHeader* next_header_by_size(BlockHeader* h, size_t blk_size) {
  return (BlockHeader*)((uint8_t*)h + blk_size);
}

// -----------------------------------------------------------------------------
// Header-only validation.
// -----------------------------------------------------------------------------
static bool header_valid_only(BlockHeader* h, size_t* out_sf) {
  if (!header_in_heap(h)) return false;

  // We need to tentatively derive payload info to check the checksum.
  // We try the primary size first.
  size_t p_sz = safe_size(h->primary.size_and_flags);
  uint8_t* pay = (uint8_t*)h + sizeof(BlockHeader);
  size_t p_len =
      (p_sz > sizeof(BlockHeader) + sizeof(BlockFooter))
          ? p_sz - sizeof(BlockHeader) - sizeof(BlockFooter)
          : 0;

  if (meta_unit_valid(&h->primary, MAGIC_HEAD, pay, p_len)) {
    *out_sf = h->primary.size_and_flags;
    if (p_sz < min_block_size() || p_sz > g_heap_size ||
        p_sz % MM_ALIGN != 0 ||
        (uint8_t*)h + p_sz > g_heap + g_heap_size) {
      return false;
    }
    return true;
  }

  // Try mirror.
  size_t m_sz = safe_size(h->mirror.size_and_flags);
  size_t m_len =
      (m_sz > sizeof(BlockHeader) + sizeof(BlockFooter))
          ? m_sz - sizeof(BlockHeader) - sizeof(BlockFooter)
          : 0;

  if (meta_unit_valid(&h->mirror, MAGIC_HEAD, pay, m_len)) {
    *out_sf = h->mirror.size_and_flags;
    if (m_sz < min_block_size() || m_sz > g_heap_size ||
        m_sz % MM_ALIGN != 0 ||
        (uint8_t*)h + m_sz > g_heap + g_heap_size) {
      return false;
    }
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Brownout-friendly metadata writes with explicit INPROG marker.
// -----------------------------------------------------------------------------
static void write_block_meta(BlockHeader* h, size_t sf) {
  size_t sz = safe_size(sf);
  BlockFooter* f = footer_by_size(h, sz);
  uint8_t* pay = payload_ptr(h);
  size_t len = payload_size_from_block(sz);

  // Stage 1: announce intent in mirrors (INPROG).
  size_t sf_inprog = sf | FLAG_INPROG;
  meta_unit_write(&h->mirror, MAGIC_HEAD, sf_inprog, pay, len);
  meta_unit_write(&f->mirror, MAGIC_FOOT, sf_inprog, pay, len);

  // Stage 2: commit stable state to primaries.
  meta_unit_write(&f->primary, MAGIC_FOOT, sf, pay, len);
  meta_unit_write(&h->primary, MAGIC_HEAD, sf, pay, len);

  // Stage 3: clear INPROG in mirrors to match primaries.
  meta_unit_write(&f->mirror, MAGIC_FOOT, sf, pay, len);
  meta_unit_write(&h->mirror, MAGIC_HEAD, sf, pay, len);
}

// -----------------------------------------------------------------------------
// Repair footer using a surviving header.
// -----------------------------------------------------------------------------
static bool repair_block_if_possible(BlockHeader* h, size_t* out_sf) {
  if (!header_in_heap(h)) return false;

  size_t hsf = 0;
  // If header invalid or explicitly marked INPROG, cannot repair.
  if (!header_valid_only(h, &hsf) || safe_inprog(hsf)) return false;

  size_t hsz = safe_size(hsf);
  BlockFooter* f = footer_by_size(h, hsz);
  if (!footer_in_heap(f)) return false;

  uint8_t* pay = payload_ptr(h);
  size_t len = payload_size_from_block(hsz);
  size_t fsf = 0;
  bool f_ok = meta_pick(&f->primary, &f->mirror, MAGIC_FOOT, &fsf, pay, len);

  if (f_ok && fsf == hsf && !safe_inprog(fsf)) {
    *out_sf = hsf;
    return true;
  }

  // Repair strategy: rewrite footer copies directly from header state.
  meta_unit_write(&f->mirror, MAGIC_FOOT, hsf, pay, len);
  meta_unit_write(&f->primary, MAGIC_FOOT, hsf, pay, len);

  *out_sf = hsf;
  return true;
}

// -----------------------------------------------------------------------------
// Full block validation.
// -----------------------------------------------------------------------------
static bool block_sane(BlockHeader* h, size_t* out_sf) {
  size_t hsf = 0;

  if (!header_valid_only(h, &hsf)) {
    return repair_block_if_possible(h, out_sf);
  }

  // If the block is in-progress, it is not "sane" for allocation use.
  if (safe_inprog(hsf)) return false;

  size_t sz = safe_size(hsf);
  BlockFooter* f = footer_by_size(h, sz);

  if (!footer_in_heap(f)) {
    return repair_block_if_possible(h, out_sf);
  }

  uint8_t* pay = payload_ptr(h);
  size_t len = payload_size_from_block(sz);
  size_t fsf = 0;

  if (meta_pick(&f->primary, &f->mirror, MAGIC_FOOT, &fsf, pay, len) &&
      !safe_inprog(fsf) && fsf == hsf) {
    *out_sf = hsf;
    return true;
  }

  return repair_block_if_possible(h, out_sf);
}

// -----------------------------------------------------------------------------
// Locate previous header.
// -----------------------------------------------------------------------------
static BlockHeader* prev_header_by_footer(BlockHeader* h) {
  if (g_heap == NULL) return NULL;
  if ((uint8_t*)h <= g_heap + sizeof(BlockFooter)) return NULL;

  BlockFooter* pf = (BlockFooter*)((uint8_t*)h - sizeof(BlockFooter));
  if (!footer_in_heap(pf)) return NULL;

  // We can't checksum payload of prev block easily here without size.
  // Use NULL payload to check metadata structure only.
  size_t psf;
  if (!meta_pick(&pf->primary, &pf->mirror, MAGIC_FOOT, &psf, NULL, 0)) {
    return NULL;
  }

  size_t psz = safe_size(psf);
  if (psz < min_block_size()) return NULL;
  if ((uint8_t*)h < g_heap + psz) return NULL;

  BlockHeader* ph = (BlockHeader*)((uint8_t*)h - psz);
  if (!header_in_heap(ph)) return NULL;

  return ph;
}

// -----------------------------------------------------------------------------
// Radiation-storm minimal survival.
// -----------------------------------------------------------------------------
static BlockHeader* resync_forward(BlockHeader* start, uint8_t* end) {
  uint8_t* p = (uint8_t*)start;
  while (p + sizeof(BlockHeader) <= end) {
    BlockHeader* h = (BlockHeader*)p;
    size_t sf;
    if (header_valid_only(h, &sf) && !safe_inprog(sf)) return h;
    p += MM_ALIGN;
  }
  return NULL;
}

// -----------------------------------------------------------------------------
// Coalescing reduces fragmentation.
// -----------------------------------------------------------------------------
static BlockHeader* coalesce(BlockHeader* h) {
  size_t hsf;
  if (!block_sane(h, &hsf)) return h;
  if (safe_alloc(hsf) || safe_bad(hsf)) return h;

  size_t sz = safe_size(hsf);

  // Attempt to merge with the next free block.
  BlockHeader* n = next_header_by_size(h, sz);
  size_t nsf;
  if (header_in_heap(n) && block_sane(n, &nsf) &&
      !safe_alloc(nsf) && !safe_bad(nsf)) {
    size_t new_sz = sz + safe_size(nsf);
    write_block_meta(h, new_sz);
    sz = new_sz;
  }

  // Attempt to merge with the previous free block.
  BlockHeader* p = prev_header_by_footer(h);
  size_t psf;
  if (p && block_sane(p, &psf) &&
      !safe_alloc(psf) && !safe_bad(psf)) {
    size_t new_sz = safe_size(psf) + sz;
    write_block_meta(p, new_sz);
    h = p;
  }

  return h;
}

// -----------------------------------------------------------------------------
// Split a free block.
// -----------------------------------------------------------------------------
static void split_block(BlockHeader* h, size_t hsf, size_t needed_total) {
  size_t sz = safe_size(hsf);

  if (sz < needed_total + min_block_size()) {
    write_block_meta(h, sz | FLAG_ALLOC);
    return;
  }

  write_block_meta(h, needed_total | FLAG_ALLOC);

  BlockHeader* n = next_header_by_size(h, needed_total);
  size_t nsz = sz - needed_total;

  write_block_meta(n, nsz);
  fill_pattern_aligned(payload_ptr(n), payload_size_from_block(nsz));
}

// -----------------------------------------------------------------------------
// Public API: init and allocation.
// -----------------------------------------------------------------------------
int mm_init(uint8_t* heap, size_t heap_size) {
  mm_lock();

  if (heap == NULL || heap_size < 256) {
    mm_unlock();
    return 1;
  }

  g_heap_base = heap;
  g_heap_total = heap_size;

  if (heap_size >= 5) {
    capture_unused_pattern(heap);
  } else {
    g_pattern_captured = false;
  }

  (void)detect_heap_pattern_sample(heap, heap_size);

  size_t hdr_size = sizeof(BlockHeader);
  size_t offset = (MM_ALIGN - (hdr_size % MM_ALIGN)) % MM_ALIGN;

  if (offset >= heap_size) {
    mm_unlock();
    return 2;
  }

  g_heap = heap + offset;
  g_heap_size = heap_size - offset;

  size_t usable = align_down(g_heap_size, MM_ALIGN);

  if (usable < min_block_size()) {
    mm_unlock();
    return 3;
  }

  fill_pattern_aligned(g_heap, g_heap_size);

  BlockHeader* h = (BlockHeader*)g_heap;
  write_block_meta(h, usable);
  fill_pattern_aligned(payload_ptr(h), payload_size_from_block(usable));

  mm_unlock();
  return 0;
}

void* mm_malloc(size_t size) {
  mm_lock();

  if (g_heap == NULL || size == 0) {
    mm_unlock();
    return NULL;
  }

  size_t req_payload = align_up(size, MM_ALIGN);
  size_t needed_total =
      sizeof(BlockHeader) + req_payload + sizeof(BlockFooter);

  needed_total = align_up(needed_total, MM_ALIGN);
  if (needed_total < min_block_size()) {
    needed_total = min_block_size();
  }

  uint8_t* p = g_heap;
  uint8_t* end = g_heap + g_heap_size;

  while (p + sizeof(BlockHeader) <= end) {
    BlockHeader* h = (BlockHeader*)p;
    size_t hsf;

    // First check if header is structurally valid (may have INPROG flag).
    if (!header_valid_only(h, &hsf)) {
      // Header corrupt. Try to resync forward.
      BlockHeader* recovered = resync_forward(h, end);
      if (recovered != NULL) {
        p = (uint8_t*)recovered;
        continue;
      }
      p += MM_ALIGN;
      continue;
    }

    // === BROWNOUT RECOVERY: INPROG blocks ===
    // If block has INPROG flag, a previous operation was interrupted.
    // The metadata is valid, so recover by deallocating the block.
    if (safe_inprog(hsf)) {
      size_t sz = safe_size(hsf);
      // Recover: clear flags and mark as free.
      write_block_meta(h, sz);  // Clear ALLOC and INPROG.
      fill_pattern_aligned(payload_ptr(h), payload_size_from_block(sz));
      // Update hsf to reflect the recovered state.
      hsf = sz;
      // Fall through to check if this block fits our allocation.
    }

    // Full block validation (header + footer consistency).
    if (!block_sane(h, &hsf)) {
      size_t sz = safe_size(hsf);
      if (sz == 0) {
        p += MM_ALIGN;
      } else {
        p += sz;
      }
      continue;
    }

    size_t sz = safe_size(hsf);

    // Block is valid and stable. Check if it fits.
    if (!safe_alloc(hsf) && !safe_bad(hsf) && sz >= needed_total) {
      split_block(h, hsf, needed_total);
      uint8_t* pay = payload_ptr(h);
      uintptr_t pay_offset = (uintptr_t)pay - (uintptr_t)g_heap_base;

      // Ensure payload alignment is consistent with MM_ALIGN.
      if (pay_offset % MM_ALIGN != 0) {
        write_block_meta(h, sz | FLAG_ALLOC | FLAG_BAD);
        mm_unlock();
        return NULL;
      }

      mm_unlock();
      return pay;
    }
    p += sz;
  }

  mm_unlock();
  return NULL;
}
void* mm_calloc(size_t nmemb, size_t size) {
  // Handle zero-size like the system allocator commonly does: return NULL.
  if (nmemb == 0 || size == 0) {
    return NULL;
  }

  // Overflow check: nmemb * size must not wrap.
  if (size != 0 && nmemb > SIZE_MAX / size) {
    return NULL;
  }

  size_t total = nmemb * size;
  void* ptr = mm_malloc(total);
  if (ptr == NULL) {
    return NULL;
  }

  // Use mm_write in small chunks so metadata and checksums are updated
  // with the brownout-safe protocol already implemented there.
  uint8_t zeros[256] = {0};
  size_t remaining = total;
  size_t offset = 0;

  while (remaining > 0) {
    size_t chunk = (remaining < sizeof(zeros)) ? remaining : sizeof(zeros);
    int written = mm_write(ptr, offset, zeros, chunk);
    if (written < 0) {
      // Something went wrong; free the block and fail.
      mm_free(ptr);
      return NULL;
    }
    remaining -= (size_t)written;
    offset += (size_t)written;
  }

  return ptr;
}
// -----------------------------------------------------------------------------
// Convert payload pointer to validated header pointer.
// IMPORTANT: This now allows INPROG blocks through so mm_read can recover them.
// -----------------------------------------------------------------------------
static BlockHeader* ptr_to_header(void* ptr, size_t* out_hsf) {
  if (ptr == NULL || g_heap == NULL) return NULL;

  uint8_t* u = (uint8_t*)ptr;

  if (u < g_heap + sizeof(BlockHeader)) return NULL;
  if (u >= g_heap + g_heap_size) return NULL;

  BlockHeader* h = (BlockHeader*)(u - sizeof(BlockHeader));
  size_t hsf = 0;

  // First: try header-only validation. This can succeed for INPROG blocks.
  if (!header_valid_only(h, &hsf)) {
    // Fallback to full sanity check for non-INPROG stable blocks.
    if (!block_sane(h, &hsf)) {
      return NULL;
    }
  } else {
    // Header looks structurally valid. If it's *not* INPROG, require
    // full block sanity to verify footer and payload checksum.
    if (!safe_inprog(hsf)) {
      size_t tmp = 0;
      if (!block_sane(h, &tmp)) {
        return NULL;
      }
      hsf = tmp;
    }
    // If it *is* INPROG, we deliberately skip block_sane because
    // block_sane rejects INPROG by design. mm_read/mm_free will handle it.
  }

  // Reject quarantined blocks.
  if (safe_bad(hsf)) return NULL;

  // Accept allocated or INPROG blocks only.
  if (!safe_alloc(hsf) && !safe_inprog(hsf)) return NULL;

  // Verify payload pointer invariants.
  if (u != payload_ptr(h)) return NULL;

  *out_hsf = hsf;
  return h;
}

// -----------------------------------------------------------------------------
// Read / Write APIs with brownout detection.
// -----------------------------------------------------------------------------
int mm_read(void* ptr, size_t offset, void* buf, size_t len) {
  mm_lock();

  if (len == 0) {
    mm_unlock();
    return 0;
  }
  if (buf == NULL) {
    mm_unlock();
    return -1;
  }

  size_t hsf;
  BlockHeader* h = ptr_to_header(ptr, &hsf);

  if (h == NULL) {
    mm_unlock();
    return -1;
  }

  // === BROWNOUT DETECTION: DURING PREVIOUS OPERATION ===
  // If INPROG flag is set, a previous write was interrupted by brownout.
  // The payload is in an inconsistent state - deallocate and return error.
  if (safe_inprog(hsf)) {
    // Brownout occurred during a previous write operation.
    // Block metadata is intact but payload is partially written garbage.
    // Deallocate block to return memory to pool.
    size_t block_size = safe_size(hsf);
    write_block_meta(h, block_size);  // Clear ALLOC and INPROG flags.
    fill_pattern_aligned(payload_ptr(h),
                         payload_size_from_block(block_size));
    (void)coalesce(h);
    mm_unlock();
    return -1;
  }

  size_t sz = safe_size(hsf);
  size_t ps = payload_size_from_block(sz);

  // Bounds check for the read operation.
  if (offset > ps || len > ps - offset) {
    mm_unlock();
    return -1;
  }

  // === BROWNOUT DETECTION: AFTER PREVIOUS OPERATION ===
  // Check for pattern-based brownout indicators in the region being read.
  // Detection:
  // 1. Entire read region matches pattern -> never written (report -1).
  // 2. Pattern appears in the middle -> partial write (brownout) -> free.
  if (g_pattern_captured && len >= 5) {
    uint8_t* pay = payload_ptr(h);
    uint8_t* read_start = pay + offset;

    // 1. Entire region looks like unused pattern -> treat as uninitialized.
    if (looks_like_pattern_aligned(read_start, len)) {
      mm_unlock();
      return -1;
    }

    // 2. Look for pattern appearing in the middle of the read.
    size_t start_phase = 0;
    if (g_heap_base != NULL) {
      start_phase = (size_t)(read_start - g_heap_base) % 5;
    }

    int starts_with_pattern = 1;
    for (size_t j = 0; j < 5; ++j) {
      if (read_start[j] != pattern_byte(start_phase + j)) {
        starts_with_pattern = 0;
        break;
      }
    }

    if (!starts_with_pattern) {
      for (size_t i = 5; i + 5 <= len; ++i) {
        int match = 1;
        for (size_t j = 0; j < 5; ++j) {
          if (read_start[i + j] != pattern_byte(start_phase + i + j)) {
            match = 0;
            break;
          }
        }
        if (match) {
          // Brownout detected: payload is partially written garbage.
          size_t block_size = safe_size(hsf);
          write_block_meta(h, block_size);  // Clear FLAG_ALLOC.
          fill_pattern_aligned(payload_ptr(h),
                               payload_size_from_block(block_size));
          (void)coalesce(h);
          mm_unlock();
          return -1;
        }
      }
    }
  }

  // All brownout checks passed - safe to read.
  memcpy(buf, payload_ptr(h) + offset, len);

  mm_unlock();
  return (int)len;
}

int mm_write(void* ptr, size_t offset, const void* src, size_t len) {
  mm_lock();

  if (len == 0) {
    mm_unlock();
    return 0;
  }
  if (src == NULL) {
    mm_unlock();
    return -1;
  }

  size_t hsf;
  BlockHeader* h = ptr_to_header(ptr, &hsf);

  if (h == NULL) {
    mm_unlock();
    return -1;
  }

  // Don't attempt to write into an INPROG/quarantined block.
  if (safe_inprog(hsf) || safe_bad(hsf)) {
    mm_unlock();
    return -1;
  }

  size_t sz = safe_size(hsf);
  size_t ps = payload_size_from_block(sz);
  if (offset > ps || len > ps - offset) {
    mm_unlock();
    return -1;
  }

  // === BROWNOUT-SAFE WRITE PROTOCOL ===
  // Stage 1: Mark block as INPROG (in-progress) before modifying payload.
  write_block_meta(h, sz | FLAG_ALLOC | FLAG_INPROG);

  // Stage 2: Perform the actual memory write.
  memcpy(payload_ptr(h) + offset, src, len);

  // Stage 3: Commit - clear INPROG and update checksum with new payload.
  write_block_meta(h, sz | FLAG_ALLOC);

  mm_unlock();
  return (int)len;
}

// -----------------------------------------------------------------------------
// Free: with brownout-aware handling of INPROG blocks.
// -----------------------------------------------------------------------------
void mm_free(void* ptr) {
  mm_lock();

  if (ptr == NULL || g_heap == NULL) {
    mm_unlock();
    return;
  }

  uint8_t* u = (uint8_t*)ptr;
  if (u < g_heap || u >= g_heap + g_heap_size) {
    mm_unlock();
    return;
  }

  BlockHeader* h = (BlockHeader*)(u - sizeof(BlockHeader));
  size_t hsf;

  // First: header-only validation so we can inspect INPROG state.
  if (!header_valid_only(h, &hsf)) {
    // Block metadata is corrupt - best-effort quarantine.
    size_t guess;
    if (meta_pick(&h->primary, &h->mirror, MAGIC_HEAD, &guess, NULL, 0)) {
      size_t sz = safe_size(guess);
      if (sz >= min_block_size() &&
          (uint8_t*)h + sz <= g_heap + g_heap_size) {
        write_block_meta(h, sz | FLAG_BAD);  // Quarantine.
      }
    }
    mm_unlock();
    return;
  }

  // === BROWNOUT RECOVERY: INPROG blocks ===
  // If INPROG flag is set, a previous write was interrupted by brownout.
  // The metadata is intact, so we can safely deallocate (not quarantine).
  if (safe_inprog(hsf)) {
    size_t sz = safe_size(hsf);
    write_block_meta(h, sz);  // Clear ALLOC and INPROG, mark as free.
    fill_pattern_aligned(payload_ptr(h), payload_size_from_block(sz));
    (void)coalesce(h);
    mm_unlock();
    return;
  }

  // Now require full block sanity for normal frees / quarantine / double-free.
  if (!block_sane(h, &hsf)) {
    // Block metadata is corrupt - best-effort quarantine.
    size_t guess;
    if (meta_pick(&h->primary, &h->mirror, MAGIC_HEAD, &guess, NULL, 0)) {
      size_t sz = safe_size(guess);
      if (sz >= min_block_size() &&
          (uint8_t*)h + sz <= g_heap + g_heap_size) {
        write_block_meta(h, sz | FLAG_BAD);  // Quarantine.
      }
    }
    mm_unlock();
    return;
  }

  // Double-free detection: block not allocated.
  if (!safe_alloc(hsf) && !safe_bad(hsf)) {
    write_block_meta(h, safe_size(hsf) | FLAG_BAD);
    mm_unlock();
    return;
  }

  // Already quarantined - ignore.
  if (safe_bad(hsf)) {
    mm_unlock();
    return;
  }

  // Normal free: deallocate block.
  size_t sz = safe_size(hsf);
  write_block_meta(h, sz);  // Clear FLAG_ALLOC.
  fill_pattern_aligned(payload_ptr(h), payload_size_from_block(sz));
  (void)coalesce(h);
  mm_unlock();
}

// -----------------------------------------------------------------------------
// Realloc + stats.
// -----------------------------------------------------------------------------
void* mm_realloc(void* ptr, size_t new_size) {
  if (ptr == NULL) return mm_malloc(new_size);
  if (new_size == 0) {
    mm_free(ptr);
    return NULL;
  }

  size_t hsf;
  BlockHeader* h = ptr_to_header(ptr, &hsf);
  if (h == NULL || safe_inprog(hsf)) {
    // Invalid pointer or block left INPROG; safer to fail.
    return NULL;
  }

  size_t old_ps = payload_size_from_block(safe_size(hsf));
  if (new_size <= old_ps) return ptr;

  void* n = mm_malloc(new_size);
  if (n == NULL) return NULL;

  memcpy(n, ptr, old_ps);
  mm_free(ptr);
  return n;
}
mm_heap_status_t mm_heap_validate(void) {
  mm_lock();

  if (g_heap == NULL) {
    mm_unlock();
    return MM_HEAP_STATUS_NOT_INITIALISED;
  }

  uint8_t* p   = g_heap;
  uint8_t* end = g_heap + g_heap_size;

  bool prev_free = false;

  while (p + sizeof(BlockHeader) <= end) {
    BlockHeader* h = (BlockHeader*)p;

    // Basic bounds check for the header itself.
    if (!header_in_heap(h)) {
      mm_unlock();
      return MM_HEAP_STATUS_OUT_OF_BOUNDS;
    }

    // Header-only validation, including payload checksum via size_and_flags.
    size_t hsf = 0;
    if (!header_valid_only(h, &hsf)) {
      mm_unlock();
      return MM_HEAP_STATUS_CORRUPT_HEADER;
    }

    size_t sz = safe_size(hsf);

    // Sanity of size/flags.
    if (sz < min_block_size() || sz % MM_ALIGN != 0) {
      mm_unlock();
      return MM_HEAP_STATUS_BAD_BLOCK_SIZE;
    }
    if ((uint8_t*)h + sz > end) {
      mm_unlock();
      return MM_HEAP_STATUS_OUT_OF_BOUNDS;
    }

    // INPROG state is an error for validation (heap not fully consistent).
    if (safe_inprog(hsf)) {
      mm_unlock();
      return MM_HEAP_STATUS_INPROG_BLOCK;
    }

    // Footer location and metadata consistency.
    BlockFooter* f = footer_by_size(h, sz);
    if (!footer_in_heap(f)) {
      mm_unlock();
      return MM_HEAP_STATUS_OUT_OF_BOUNDS;
    }

    uint8_t* pay = payload_ptr(h);
    size_t   len = payload_size_from_block(sz);

    size_t fsf = 0;
    if (!meta_pick(&f->primary, &f->mirror, MAGIC_FOOT, &fsf, pay, len)) {
      mm_unlock();
      return MM_HEAP_STATUS_CORRUPT_FOOTER;
    }
    if (safe_inprog(fsf)) {
      mm_unlock();
      return MM_HEAP_STATUS_INPROG_BLOCK;
    }
    if (fsf != hsf) {
      mm_unlock();
      return MM_HEAP_STATUS_CORRUPT_FOOTER;
    }

    // Free-block specific checks.
    bool is_alloc = safe_alloc(hsf);
    bool is_bad   = safe_bad(hsf);

    if (!is_alloc && !is_bad) {
      // 1) Free blocks must have their payload filled with the pattern.
      if (g_pattern_captured) {
        if (!looks_like_pattern_aligned(pay, len)) {
          mm_unlock();
          return MM_HEAP_STATUS_PATTERN_MISMATCH;
        }
      }

      // 2) No two adjacent free blocks (coalescing invariant).
      if (prev_free) {
        mm_unlock();
        return MM_HEAP_STATUS_ADJACENT_FREE;
      }
      prev_free = true;
    } else {
      prev_free = false;
    }

    // Advance to next block.
    p += sz;
  }

  // If we reach here, the heap looks consistent according to all checks.
  mm_unlock();
  return MM_HEAP_STATUS_OK;
}

void mm_heap_stats(void) {
  mm_lock();

  if (g_heap == NULL) {
    printf("Heap not initialised\n");
    mm_unlock();
    return;
  }

  size_t used = 0;
  size_t freeb = 0;
  size_t bad = 0;
  size_t blocks = 0;

  uint8_t* p = g_heap;
  uint8_t* end = g_heap + g_heap_size;

  while (p + sizeof(BlockHeader) <= end) {
    BlockHeader* h = (BlockHeader*)p;
    size_t hsf;

    // If header is fine but block is INPROG, treat it as "bad" in stats.
    if (header_valid_only(h, &hsf) && safe_inprog(hsf)) {
      size_t sz = safe_size(hsf);
      blocks++;
      bad += sz;
      p += sz;
      continue;
    }

    if (!block_sane(h, &hsf)) {
      BlockHeader* recovered = resync_forward(h, end);
      if (recovered != NULL) {
        p = (uint8_t*)recovered;
        continue;
      }
      p += MM_ALIGN;
      continue;
    }

    size_t sz = safe_size(hsf);
    if (sz == 0) break;

    blocks++;
    if (safe_bad(hsf)) {
      bad += sz;
    } else if (safe_alloc(hsf)) {
      used += sz;
    } else {
      freeb += sz;
    }
    p += sz;
  }

  printf("Blocks: %zu | Used: %zu | Free: %zu | Bad: %zu | Heap: %zu\n",
         blocks, used, freeb, bad, g_heap_size);

  mm_unlock();
}
