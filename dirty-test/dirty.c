#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

const uint64_t PM_SOFT_DIRTY = (uint64_t)1 << 55;
const uint64_t PM_MMAP_EXCLUSIVE = (uint64_t)1 << 56;
const uint64_t PM_UFFD_WP = (uint64_t)1 << 57;
const uint64_t PM_FILE = (uint64_t)1 << 61;
const uint64_t PM_SWAP = (uint64_t)1 << 62;
const uint64_t PM_PRESENT = (uint64_t)1 << 63;

const long log_urbit_page_size = 12;  // 16KiB page
const long urbit_page_size = 1 << log_urbit_page_size;
long log_page_size;
long page_size;
// uint64_t mask;

const uint64_t chunk_pages = 64 * 1024;

pthread_t* save_threads;
size_t save_threads_size;

int pagemap;

int loom_file;
void* loom_writethrough;
void* const loom = (void*)0x200000000;
const size_t loom_size = 0x80000000;  // 2 GiB

// const size_t num_writes = 1000;
#define NUM_WRITES 1000

int64_t difftimespec_ns(const struct timespec after, const struct timespec before) {
    return ((int64_t)after.tv_sec - (int64_t)before.tv_sec) * (int64_t)1000000000 +
           ((int64_t)after.tv_nsec - (int64_t)before.tv_nsec);
}

static inline bool pte_dirty(const uint64_t pte) {
    uint64_t masked = pte & (PM_PRESENT | PM_SWAP | PM_FILE | PM_MMAP_EXCLUSIVE);
    return masked == (PM_MMAP_EXCLUSIVE | PM_PRESENT) || masked == PM_SWAP;
}

static inline void clean_loom(void) {
    assert(MAP_FAILED !=
           mmap(loom, loom_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, loom_file, 0));
}

// base-2 log of x, where x > 0
static inline unsigned long int_log2(unsigned long x) {
    return __builtin_clzl(1) - __builtin_clzl(x);
}

void fill_random(char* buf, size_t len) {
    while (len) {
        ssize_t ret = getrandom(buf, len, 0);
        assert(ret != -1);
        buf += ret;
        len -= ret;
    }
}

/*uint64_t save_page_range(size_t start, size_t len) {
  uint64_t buf[64*1024]; // TODO: optimize this buffer's size
  size_t remaining = len * sizeof(uint64_t);
  size_t buf_start = start * sizeof(uint64_t);
  while (remaining > 0) {
    ssize_t ret = pread(pagemap, buf, MIN(remaining, sizeof(buf)), loom_seek + buf_start);
    assert(ret > 0);
    remaining -= ret;

    ssize_t pages_read = ret / sizeof(uint64_t);
    // TODO: triggering this may be impossible the way pagemap is currently implemented
    assert(pages_read * sizeof(uint64_t) == bytes_read);

    for (size_t i = 0; i < pages_read; i++) {
      if (pte_dirty(buf[i])) {
        uintptr_t offset = (buf_start + i) << log_page_size;
        memcpy(loom_writethrough+offset, loom+offset, page_size);
        count++;
        // TODO: skip
        //i |= mask;
      }
    }

    buf_start += pages_read;
  }
}*/

void* save_chunk(void* chunk) {
    uint64_t buf[chunk_pages];  // TODO: optimize this buffer's size

    void* start = buf;
    size_t remaining = chunk_pages * sizeof(uint64_t);
    off_t seek = ((uintptr_t)chunk >> log_page_size) * sizeof(uint64_t);

    do {
        ssize_t ret = pread(pagemap, start, remaining, seek);
        assert(ret > 0);
        remaining -= ret;
        start += ret;
        seek += ret;
    } while (remaining);

    for (size_t i = 0; i < chunk_pages; i++) {
        buf[i] = pte_dirty(buf[i]);
    }

    // TODO
    uintptr_t count = 0;
    ptrdiff_t writethrough_offset = loom_writethrough - loom;
    for (size_t i = 0; i < chunk_pages; i++) {
        if (buf[i]) {
            // TODO: round to urbit page sizes
            void* page = chunk + (i << log_page_size);
            // memcpy(page+writethrough_offset, page, page_size);
            count++;  // TODO
                      // TODO: skip
                      // i |= mask;
        }
    }

    return (void*)count;
}

uintptr_t save_loom(void) {
    // TODO: BUF_SIZE constant; and is this even a good way to split work?
    for (size_t thread = 0; thread < save_threads_size; thread++) {
        ptrdiff_t offset = thread * (chunk_pages << log_page_size);
        assert(pthread_create(save_threads + thread, NULL, save_chunk, loom + offset) == 0);
    }

    uintptr_t count = 0;
    for (size_t thread = 0; thread < save_threads_size; thread++) {
        uintptr_t thread_count;
        assert(pthread_join(save_threads[thread], (void**)(&thread_count)) == 0);
        count += thread_count;
    }

    assert(msync(loom_writethrough, loom_size, MS_SYNC) != -1);

    // do {
    //   struct timespec start, end;
    //   clock_gettime(CLOCK_MONOTONIC, &start);
    //
    //   ssize_t bytes_read = pread(pagemap, buf, MIN(left, sizeof(buf)));
    //   assert(bytes_read > 0);
    //
    //   clock_gettime(CLOCK_MONOTONIC, &end);
    //   printf("read took %" PRIi64 " ns\n", difftimespec_ns(end, start));
    //
    //   clock_gettime(CLOCK_MONOTONIC, &start);
    //
    //   ssize_t pages_read = bytes_read / sizeof(uint64_t);
    //   assert(pages_read * sizeof(uint64_t) == bytes_read);
    //
    //   // TODO: special case for page_size = 4096
    //
    //   /*for (size_t i = 0; i < pages_read; i++) {
    //     pte_dirty(buf[i])
    //   }*/
    //
    //   for (size_t i = 0; i < pages_read; i++) {
    //     if (pte_dirty(buf[i])) {
    //       uintptr_t offset = (page + i) << log_page_size;
    //       memcpy(loom_writethrough+offset, loom+offset, page_size);
    //       count++;
    //       i |= mask;
    //     }
    //   }
    //
    //   page += pages_read;
    //   left -= bytes_read;
    // } while (left);

    return count;  // TODO: only keeping track of count for testing
}

int main(char* argc, char** argv) {
    puts("initializing loom");

    page_size = sysconf(_SC_PAGESIZE);
    log_page_size = int_log2(page_size);
    assert(1 << log_page_size == page_size);

    save_threads_size = (loom_size >> log_page_size) / chunk_pages;
    save_threads = malloc(save_threads_size * sizeof(pthread_t));
    printf("using %d save threads\n", (int)save_threads_size);

    pagemap = open("/proc/self/pagemap", O_RDONLY);
    assert(pagemap != -1);

    // loom_file = open("loom.img", O_CREAT | O_RDWR, 0644);
    loom_file = memfd_create("loom", 0);
    assert(loom_file != -1);
    ftruncate(loom_file, loom_size);

    clean_loom();

    loom_writethrough = mmap(loom_writethrough, loom_size, PROT_WRITE, MAP_SHARED, loom_file, 0);
    assert(loom_writethrough != MAP_FAILED);

    /*printf("writing up to %zu\n", loom_size/2 / page_size);
    fill_random(loom, loom_size/2);

    printf("reading up to %zu\n", loom_size / page_size);
    uint64_t sum = 0;
    char* lc = (char*)loom;
    for (size_t i = loom_size/2; i < loom_size; i++) {
      sum += lc[i];
    }
    printf("sum %d\n", sum);*/

    puts("scanning pagemap");
    char* lc = loom;

    static size_t indices[NUM_WRITES];

    for (size_t i = 0; i < 128; i++) {
        fill_random(indices, NUM_WRITES * sizeof(size_t));
        for (size_t j = 0; j < NUM_WRITES; j++) {
            size_t r = indices[j];
            lc[r % loom_size] += r / loom_size;
        }
        // memset(loom+(i*page_size*100), i, 100*page_size);

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        uintptr_t count = save_loom();
        // clean_loom();

        clock_gettime(CLOCK_MONOTONIC, &end);
        printf("%" PRIuPTR " dirty out of %" PRIu64 " pages scanned in %" PRIi64 " ns\n", count,
               loom_size >> log_page_size, difftimespec_ns(end, start));
    }

    close(pagemap);
    // close(clear_refs);
    munmap(loom_writethrough, loom_size);
    munmap(loom, loom_size);
    close(loom_file);
    free(save_threads);

    return 0;
}
