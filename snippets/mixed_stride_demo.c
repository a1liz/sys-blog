#include <stdio.h>
#include <stdlib.h>

// A generic "process element" function that loads from *p.
// The SAME load instruction (PC) inside this function will see
// completely different stride patterns depending on the caller.
//
// Phase A: caller walks forward with step=+4  → stride = +4
// Phase B: caller walks backward with step=-2 → stride = -2
//
// Both phases execute the IDENTICAL mov instruction at the same PC.

__attribute__((noinline))
int load_element(int *p) {
    return *p;  // <--- THIS is the load PC we care about
}

int main() {
    int data[32];
    for (int i = 0; i < 32; i++) data[i] = i;

    volatile int sink;  // prevent optimization

    // Phase A: forward stride +4  (i += 4)
    printf("Phase A: forward stride +4\n");
    for (int i = 0; i < 32; i += 4) {
        sink = load_element(&data[i]);
        printf("  i=%d  addr=%p  val=%d\n", i, (void*)&data[i], sink);
    }

    // Phase B: backward stride -2  (i -= 2)
    printf("Phase B: backward stride -2\n");
    for (int i = 31; i >= 0; i -= 2) {
        sink = load_element(&data[i]);
        printf("  i=%d  addr=%p  val=%d\n", i, (void*)&data[i], sink);
    }

    return 0;
}
