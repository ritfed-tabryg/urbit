#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int64_t difftimespec_ns(const struct timespec after, const struct timespec before) {
    return ((int64_t)after.tv_sec - (int64_t)before.tv_sec) * (int64_t)1000000000 +
           ((int64_t)after.tv_nsec - (int64_t)before.tv_nsec);
}

int main(char* argc, char** argv) {
    char* loom = malloc((size_t)1024 * (size_t)1024 * (size_t)1024 * (size_t)2);
    memset(loom, 0x42, sizeof(loom));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (unsigned int i = 0; i < 20000; i++) {
        if (fork() == 0) {
            return 0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("%09" PRIi64 " ns / 20000\n", difftimespec_ns(end, start));
}
