// worker.c — implementation in SEPARATE translation unit
// gcc CANNOT inline across .o files without LTO
#include "worker.h"

int sum_strided(int *arr, int n, int stride) {
    int sum = 0;
    for (int i = 0; i < n; i += stride) {
        sum += arr[i];  // ← LOAD PC: stride varies per caller
    }
    return sum;
}

int sum_indirect(int *arr, int *indices, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[indices[i]];  // ← LOAD PC: sequential if indices[i]==i
    }                             //            random if indices is shuffled
    return sum;
}
