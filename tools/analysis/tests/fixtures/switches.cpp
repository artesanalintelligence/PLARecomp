// Functions with dense switches so the compiler generates jump tables.
extern "C" int sw_small(int x, int y) {
    switch (x) {
    case 0: return y + 1;
    case 1: return y * 3;
    case 2: return y - 7;
    case 3: return y ^ 0x55;
    case 4: return y << 2;
    case 5: return y / 5;
    case 6: return y % 9;
    case 7: return -y;
    default: return 0;
    }
}

extern "C" __attribute__((noinline)) int sink(int);

extern "C" int sw_large(unsigned x, int y) {
    switch (x) {
#define C(n) case n: return sink(y + n * 13);
    C(0) C(1) C(2) C(3) C(4) C(5) C(6) C(7) C(8) C(9) C(10) C(11) C(12) C(13) C(14)
    C(15) C(16) C(17) C(18) C(19) C(20) C(21) C(22) C(23) C(24) C(25) C(26) C(27)
    C(28) C(29) C(30) C(31) C(32) C(33) C(34) C(35) C(36) C(37) C(38) C(39)
#undef C
    default: return -1;
    }
}

extern "C" __attribute__((noinline)) int sink(int v) { return v * 2 + 1; }

typedef int (*fnptr)(int);
static int cb_a(int v) { return v + 100; }
static int cb_b(int v) { return v - 100; }
// Table of function pointers in .data.rel.ro (R_AARCH64_RELATIVE to .text)
extern "C" const fnptr g_callbacks[] = {cb_a, cb_b, sink};
