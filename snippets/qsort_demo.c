#include <stdio.h>
#include <stdlib.h>

int cmp_func(const void *a, const void *b) {
    int va = *(const int *)a;
    int vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

int main() {
    int data[512];
    for (int i = 0; i < 512; i++) data[i] = (i * 7 + 13) % 512;
    printf("Before sort: data[0]=%d data[511]=%d\n", data[0], data[511]);
    qsort(data, 512, sizeof(int), cmp_func);
    printf("After sort:  data[0]=%d data[511]=%d\n", data[0], data[511]);
    return 0;
}
