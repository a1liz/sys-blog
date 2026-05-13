#include <stdio.h>
#include <stdlib.h>

// SAME logic as before, but WITHOUT noinline.
// The compiler is free to inline this → different PCs at each call site.
int load_element(int *p) {
    return *p;
}

int main() {
    int data[32];
    for (int i = 0; i < 32; i++) data[i] = i;

    volatile int sink;

    printf("Phase A: forward stride +4\n");
    for (int i = 0; i < 32; i += 4) {
        sink = load_element(&data[i]);
        printf("  i=%d  addr=%p  val=%d\n", i, (void*)&data[i], sink);
    }

    printf("Phase B: backward stride -2\n");
    for (int i = 31; i >= 0; i -= 2) {
        sink = load_element(&data[i]);
        printf("  i=%d  addr=%p  val=%d\n", i, (void*)&data[i], sink);
    }

    return 0;
}
