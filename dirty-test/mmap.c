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

int64_t difftimespec_ns(const struct timespec after, const struct timespec before) {
    return ((int64_t)after.tv_sec - (int64_t)before.tv_sec) * (int64_t)1000000000 +
           ((int64_t)after.tv_nsec - (int64_t)before.tv_nsec);
}

int main(char* argc, char** argv) {
    size_t loom_size = 0x80000000;  // 2 GiB
    // int loom_file = open("loom.img", O_CREAT | O_RDWR, 0644);
    int loom_file = memfd_create("loom", 0);
    ftruncate(loom_file, loom_size);

    void* loom = (void*)0x200000000;
    assert(MAP_FAILED !=
           mmap(loom, loom_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, loom_file, 0));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (unsigned int i = 0; i < 20000; i++) {
        assert(MAP_FAILED != mmap(loom, loom_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE,
                                  loom_file, 0));
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("%09" PRIi64 " ns / 20000\n", difftimespec_ns(end, start));
}
