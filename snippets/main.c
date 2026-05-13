#include <stdio.h>
#include <stdlib.h>
#include "worker.h"

int main() {
    int data[512];
    int indices[512];
    for (int i = 0; i < 512; i++) data[i] = (i * 7 + 13) % 512;

    // --- sum_strided: same PC, 3 different strides ---
    printf("=== sum_strided: 3 strides, 1 PC ===\n");
    volatile int r1 = sum_strided(data, 512, 1);
    printf("stride=1: %d\n", r1);
    volatile int r2 = sum_strided(data, 512, 4);
    printf("stride=4: %d\n", r2);
    volatile int r3 = sum_strided(data, 512, 16);
    printf("stride=16: %d\n", r3);

    // --- sum_indirect: same PC, seq vs random ---
    printf("=== sum_indirect: seq vs random, same PC ===\n");
    for (int i = 0; i < 512; i++) indices[i] = i;
    volatile int r4 = sum_indirect(data, indices, 512);
    printf("sequential indices: %d\n", r4);

    // shuffle indices
    for (int i = 0; i < 512; i++) {
        int j = rand() % 512;
        int t = indices[i]; indices[i] = indices[j]; indices[j] = t;
    }
    volatile int r5 = sum_indirect(data, indices, 512);
    printf("random indices: %d\n", r5);

    return 0;
}
