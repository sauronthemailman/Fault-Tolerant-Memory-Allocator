/* full_benchmark.c
 * Mars Rover Allocator: Comprehensive Benchmark Suite
 * ===================================================
 * A unified test harness containing:
 * 1. Payload Efficiency Analysis
 * 2. Allocation vs Deallocation Latency
 * 3. Read/Write Throughput vs Hardware Baseline
 * 4. "Mars Mission" Sustained Stress Test
 *
 * Constraints:
 * - 1MB Heap (Embedded System Constraint)
 * - Monotonic Timer (High Precision)
 * - Radiation & Brownout Fault Injection
 *
 * Compile: gcc -o full_benchmark full_benchmark.c -L. -lallocator
 *          -Wl,-rpath,'$ORIGIN'
 * Run:     ./full_benchmark
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "allocator.h"

/* CONSTANTS */
#define HEAP_CAPACITY       (1024 * 1024)  /* 1MB Rover Constraint */
#define RW_ITERATIONS       10000  /* Throughput iterations */
#define MISSION_CYCLES      1000000L  /* 1 Million Ops for Stress Test */
#define MAX_TRACKED_BLOCKS  1024  /* Max active allocations */

/* GLOBAL STATE */
static uint8_t* g_heap_base = NULL;

/* UTILITIES */

double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

void reset_heap_sector() {
  const uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA};
  for (size_t i = 0; i < HEAP_CAPACITY; i++) {
    g_heap_base[i] = pattern[i % 5];
  }
  mm_init(g_heap_base, HEAP_CAPACITY);
}

const char* status_str(mm_heap_status_t st) {
  switch (st) {
    case MM_HEAP_STATUS_OK:
      return "OK (NOMINAL)";
    case MM_HEAP_STATUS_NOT_INITIALISED:
      return "NOT_INIT";
    case MM_HEAP_STATUS_CORRUPT_HEADER:
      return "CORRUPT_HEADER";
    case MM_HEAP_STATUS_CORRUPT_FOOTER:
      return "CORRUPT_FOOTER";
    case MM_HEAP_STATUS_BAD_BLOCK_SIZE:
      return "BAD_SIZE";
    case MM_HEAP_STATUS_OUT_OF_BOUNDS:
      return "OUT_OF_BOUNDS";
    case MM_HEAP_STATUS_INPROG_BLOCK:
      return "INPROG_BLOCK";
    case MM_HEAP_STATUS_ADJACENT_FREE:
      return "ADJACENT_FREE";
    case MM_HEAP_STATUS_PATTERN_MISMATCH:
      return "PATTERN_MISMATCH";
    default:
      return "UNKNOWN_ERROR";
  }
}

/* MODULE 1: PAYLOAD EFFICIENCY ANALYSIS (UPDATED) */
void run_efficiency_test() {
  printf("\n[MODULE 1] Payload Efficiency Analysis\n");
  printf("Request (B)   Block (B)     Overhead (B)   Efficiency\n");
  printf("-----------------------------------------------------\n");

  size_t sizes[] = {
    8, 16, 24, 32, 48, 64, 96, 128,
    192, 256, 384, 512, 768, 1024,
    1536, 2048, 3072, 4096
  };
  int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  reset_heap_sector();

  for (int i = 0; i < num_sizes; i++) {
    size_t req = sizes[i];

    void* p1 = mm_malloc(req);
    void* p2 = mm_malloc(req);

    if (p1 && p2) {
      size_t actual_size = (uint8_t*)p2 - (uint8_t*)p1;
      size_t overhead = actual_size - req;
      double eff = 100.0 * req / actual_size;

      printf("%-13zu %-13zu %-14zu %.1f%%\n",
             req, actual_size, overhead, eff);
      mm_free(p2);
      mm_free(p1);
    } else {
      /* If alloc fails (e.g., too big for current fragmentation), stop */
      break;
    }
  }
}

/* MODULE 2: LATENCY ANALYSIS (Alloc vs Free) */
void run_latency_test(size_t block_size) {
  reset_heap_sector();
  static void* ptrs[20000];
  size_t count = 0;

  /* 1. Dry Run */
  while (count < 20000) {
    void* p = mm_malloc(block_size);
    if (!p) {
      break;
    }
    ptrs[count++] = p;
  }
  for (size_t i = 0; i < count; i++) {
    mm_free(ptrs[i]);
  }

  /* 2. Timed Alloc */
  reset_heap_sector();
  double start = get_time_ms();
  for (size_t i = 0; i < count; i++) {
    ptrs[i] = mm_malloc(block_size);
  }
  double t_alloc = get_time_ms() - start;

  /* 3. Timed Free */
  start = get_time_ms();
  for (size_t i = 0; i < count; i++) {
    mm_free(ptrs[i]);
  }
  double t_free = get_time_ms() - start;

  double us_alloc = (t_alloc * 1000.0) / count;
  double us_free = (t_free * 1000.0) / count;

  printf("%-10zu %-10zu %-12.3f %-12.3f\n",
         block_size, count, us_alloc, us_free);
}

void suite_latency() {
  printf("\n[MODULE 2] Latency Analysis (Microseconds)\n");
  printf("Size (B)    Count       Alloc (us)   Free (us)\n");
  printf("----------------------------------------------\n");
  size_t sizes[] = {32, 64, 128, 256, 512, 1024};
  for (int i = 0; i < 6; i++) {
    run_latency_test(sizes[i]);
  }
}

/* MODULE 3: THROUGHPUT ANALYSIS */
void suite_throughput() {
  reset_heap_sector();
  size_t buf_sz = 64 * 1024;
  uint8_t* host_buf = malloc(buf_sz);
  memset(host_buf, 0xAA, buf_sz);

  printf("\n[MODULE 3] I/O Throughput vs Hardware Baseline\n");
  printf("Size (B)    Read MB/s    Write MB/s   Memcpy MB/s\n");
  printf("-----------------------------------------------\n");

  size_t sizes[] = {32, 64, 128, 256, 512, 1024, 4096};

  for (int i = 0; i < 7; i++) {
    size_t sz = sizes[i];
    void* ptr = mm_malloc(sz);
    if (!ptr) {
      break;
    }

    /* WRITE */
    double start = get_time_ms();
    for (int j = 0; j < RW_ITERATIONS; j++) {
      if (mm_write(ptr, 0, host_buf, sz) < 0) {
        exit(1);
      }
    }
    double t_write = get_time_ms() - start;
    double mb_write = ((double)(sz * RW_ITERATIONS) / (1024.0 * 1024.0)) /
                      (t_write / 1000.0);

    /* READ */
    start = get_time_ms();
    for (int j = 0; j < RW_ITERATIONS; j++) {
      if (mm_read(ptr, 0, host_buf, sz) < 0) {
        exit(1);
      }
    }
    double t_read = get_time_ms() - start;
    double mb_read = ((double)(sz * RW_ITERATIONS) / (1024.0 * 1024.0)) /
                     (t_read / 1000.0);

    /* MEMCPY */
    start = get_time_ms();
    for (int j = 0; j < RW_ITERATIONS; j++) {
      memcpy(ptr, host_buf, sz);
    }
    double t_cpy = get_time_ms() - start;
    if (t_cpy < 0.001) {
      t_cpy = 0.001;
    }
    double mb_cpy = ((double)(sz * RW_ITERATIONS) / (1024.0 * 1024.0)) /
                    (t_cpy / 1000.0);

    mm_free(ptr);
    printf("%-10zu %-12.1f %-12.1f %-12.0f\n",
           sz, mb_read, mb_write, mb_cpy);
  }
  free(host_buf);
}

/* MODULE 4: MARS MISSION STRESS TEST */

typedef struct {
  void* addr;
  size_t len;
} ManifestEntry;

typedef struct {
  long alloc_success;
  long alloc_fail;
  long free_ops;
  long read_ops;
  long write_ops;
  long corruption_events;
  long radiation_spikes;
  long power_surges;
} MissionStats;

void trigger_radiation_spike(int intensity, MissionStats* stats) {
  for (int i = 0; i < intensity; i++) {
    g_heap_base[rand() % HEAP_CAPACITY] ^= (1 << (rand() % 8));
  }
  stats->radiation_spikes++;
}

void trigger_power_surge(ManifestEntry* block, MissionStats* stats) {
  if (!block || !block->addr || block->len < 16) {
    return;
  }
  uint8_t* raw = (uint8_t*)block->addr;
  size_t start = block->len / 2;
  size_t phase = (size_t)(raw + start - g_heap_base) % 5;
  const uint8_t pat[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA};
  for (size_t i = 0; i < 16 && (start + i) < block->len; i++) {
    raw[start + i] = pat[(phase + i) % 5];
  }
  stats->power_surges++;
}

void dispatch_mission_task(ManifestEntry* manifest, int* active_count,
                           MissionStats* stats, bool hazard_mode) {
  if (hazard_mode) {
    if (rand() % 10000 == 0) {
      trigger_radiation_spike(50, stats);
    }
    if (*active_count > 0 && rand() % 40000 == 0) {
      trigger_power_surge(&manifest[rand() % *active_count], stats);
    }
  }

  int roll = rand() % 100;
  if (roll < 40) {
    /* Alloc */
    if (*active_count < MAX_TRACKED_BLOCKS) {
      size_t sz = (rand() % 10 == 0) ?
                  (rand() % 512 + 256) : (rand() % 64 + 32);
      void* p = mm_malloc(sz);
      if (p) {
        uint8_t buf[1024];
        memset(buf, 0xCC, (sz < 1024) ? sz : 1024);
        mm_write(p, 0, buf, sz);
        manifest[*active_count] = (ManifestEntry){p, sz};
        (*active_count)++;
        stats->alloc_success++;
      } else {
        stats->alloc_fail++;
      }
    }
  } else if (roll < 70) {
    /* Free */
    if (*active_count > 0) {
      int idx = rand() % *active_count;
      mm_free(manifest[idx].addr);
      manifest[idx] = manifest[--(*active_count)];
      stats->free_ops++;
    }
  } else if (roll < 85) {
    /* Read */
    if (*active_count > 0) {
      int idx = rand() % *active_count;
      uint8_t buf[1024];
      size_t len = (manifest[idx].len > 1024) ?
                   1024 : manifest[idx].len;
      if (mm_read(manifest[idx].addr, 0, buf, len) < 0) {
        stats->corruption_events++;
        mm_free(manifest[idx].addr);
        manifest[idx] = manifest[--(*active_count)];
      } else {
        stats->read_ops++;
      }
    }
  } else {
    /* Write */
    if (*active_count > 0) {
      int idx = rand() % *active_count;
      uint8_t buf[1024];
      memset(buf, rand(), 1024);
      size_t len = (manifest[idx].len > 1024) ?
                   1024 : manifest[idx].len;
      if (mm_write(manifest[idx].addr, 0, buf, len) < 0) {
        stats->corruption_events++;
      } else {
        stats->write_ops++;
      }
    }
  }
}

void execute_mission(const char* mission_name, bool hazards_enabled) {
  printf("\n[MISSION START] %s\n", mission_name);
  printf("   > Environment: %s\n",
         hazards_enabled ? "HOSTILE (Radiation Active)" : "STABLE");
  reset_heap_sector();
  static ManifestEntry manifest[MAX_TRACKED_BLOCKS];
  int active = 0;
  MissionStats stats = {0};

  for (long cycle = 0; cycle < MISSION_CYCLES; cycle++) {
    dispatch_mission_task(manifest, &active, &stats, hazards_enabled);
  }

  uint8_t dummy[16];
  for (int i = 0; i < active; i++) {
    size_t read_len = (manifest[i].len > 16) ? 16 : manifest[i].len;
    if (mm_read(manifest[i].addr, 0, dummy, read_len) < 0) {
      stats.corruption_events++;
    }
    mm_free(manifest[i].addr);
  }

  mm_heap_status_t final = mm_heap_validate();
  printf("\n[MISSION REPORT] Summary Statistics\n");
  printf("------------------------------------------------\n");
  printf("  Total Cycles       : %ld\n", MISSION_CYCLES);
  printf("  Env. Hazards       : %ld Radiation / %ld Power Surges\n",
         stats.radiation_spikes, stats.power_surges);
  printf("\n  [Operations]\n");
  printf("    Deploy (Alloc)   : %ld (Failed: %ld)\n",
         stats.alloc_success, stats.alloc_fail);
  printf("    Recall (Free)    : %ld\n", stats.free_ops);
  printf("    Telemetry (Read) : %ld\n", stats.read_ops);
  printf("    Update (Write)   : %ld\n", stats.write_ops);
  printf("\n  [Resilience]\n");
  printf("    Corruptions Caught : %ld\n", stats.corruption_events);
  printf("    Final Heap State   : %s\n", status_str(final));
  printf("------------------------------------------------\n");
}

int main() {
  srand(12345);
  g_heap_base = (uint8_t*)malloc(HEAP_CAPACITY);
  if (!g_heap_base) {
    return 1;
  }

  printf("==================================================\n");
  printf("   MARS ROVER ALLOCATOR: FULL BENCHMARK SUITE     \n");
  printf("==================================================\n");

  run_efficiency_test();
  suite_latency();
  suite_throughput();

  execute_mission("Orbital Test (Control)", false);
  execute_mission("Surface Ops (Stress)", true);

  free(g_heap_base);
  return 0;
}
