#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
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

const void* loom = (void*)0x200000000;
const size_t loom_size = 0x80000000;  // 2 GiB

int64_t difftimespec_ns(const struct timespec after, const struct timespec before) {
    return ((int64_t)after.tv_sec - (int64_t)before.tv_sec) * (int64_t)1000000000 +
           ((int64_t)after.tv_nsec - (int64_t)before.tv_nsec);
}

inline bool pte_dirty(const uint64_t pte) {
    uint64_t masked = pte & (PM_PRESENT | PM_SWAP | PM_FILE | PM_MMAP_EXCLUSIVE);
    return masked == (PM_MMAP_EXCLUSIVE | PM_PRESENT) || masked == PM_SWAP;
}

inline void clean_loom(void) {
    assert(MAP_FAILED !=
           mmap(loom, loom_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, loom_file, 0));
}

int main(char* argc, char** argv) {
    puts("initializing loom");

    long page_size = sysconf(_SC_PAGESIZE);

    // int loom_file = open("loom.img", O_CREAT | O_RDWR, 0644);
    int loom_file = memfd_create("loom", 0);
    ftruncate(loom_file, loom_size);

    clean_loom();

    // puts("clearing refs");
    // int clear_refs = open("/proc/self/clear_refs", O_WRONLY);
    // write(clear_refs, "4", 1);

    //((char*)loom)[1] = 0;
    //((char*)loom)[31337] = 0;
    printf("writing up to %zu\n", loom_size / 2 / page_size);
    {
        char* l = loom;
        size_t left = loom_size / 2;
        do {
            ssize_t ret = getrandom(l, left, 0);
            assert(ret != -1);
            left -= ret;
            l += ret;
        } while (left);
    }

    printf("reading up to %zu\n", loom_size / page_size);
    uint64_t sum = 0;
    char* lc = (char*)loom;
    for (size_t i = loom_size / 2; i < loom_size; i++) {
        sum += lc[i];
    }
    printf("sum %d\n", sum);
    // int ec = ((char*)loom)[16384];

    puts("scanning pagemap");

    int pagemap = open("/proc/self/pagemap", O_RDONLY);
    assert(-1 != lseek(pagemap, (uintptr_t)loom / page_size * sizeof(uint64_t), SEEK_SET));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t last = UINT64_MAX;
    size_t last_i = 0;

    size_t page = 0;
    size_t left = loom_size / page_size * sizeof(uint64_t);
    uint64_t buf[128 * 1024];
    do {
        ssize_t bytes_read = read(pagemap, buf, MIN(left, sizeof(buf)));
        assert(bytes_read > 0);

        ssize_t pages_read = bytes_read / sizeof(uint64_t);
        assert(pages_read * sizeof(uint64_t) == bytes_read);

        while (i < pages_read) {
            if (pte_dirty(buf[i])) {
                while (i < pages_read &&) }
        }
        for (size_t i = 0; i < pages_read; i++) {
            if (pte_dirty(buf[i])) {
            }

            /*uint64_t pte = buf[i];
            if (pte != last) {
              if (last != UINT64_MAX) {
                printf(
                  "pages %zu-%zu: 0x%" PRIx64 "%s\n",
                  last_i, page+i-1, last, pte_dirty(last) ? " (dirty)" : ""
                );
              }
              last = pte;
              last_i = page+i;
            }*/
            /*printf("page %zu: 0x%" PRIx64 "\n", i, buf[i]);
            if (buf[i] & ((uint64_t)1 << 54)) {
              printf("page %zu dirty\n", page+i);
            }*/
        }

        page += pages_read;
        left -= bytes_read;
    } while (left);

    printf("pages %zu-%zu: 0x%" PRIx64 "\n", last_i, page - 1, last);

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("scanned in %" PRIi64 " ns\n", difftimespec_ns(end, start));

    close(pagemap);
    // close(clear_refs);
    munmap(loom, loom_size);
    close(loom_file);

    return 0;
}
