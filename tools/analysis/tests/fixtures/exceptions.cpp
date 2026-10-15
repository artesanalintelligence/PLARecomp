// Functions with try/catch and non-trivial destructors: they generate LSDA
// (.gcc_except_table) with landing pads. Only compiled with -fexceptions.
#if __EXCEPTIONS
extern "C" __attribute__((noinline)) void may_throw(int v) {
    if (v < 0) throw v;
}

volatile int g_cleanups;

struct Guard {
    ~Guard() { g_cleanups = g_cleanups + 1; }  // observable effect: requires a landing pad
};

extern "C" int eh_catch(int v) {
    try {
        may_throw(v);
        return 0;
    } catch (int e) {
        return e;
    }
}

extern "C" int eh_cleanup(int v) {
    {
        Guard g;
        may_throw(v);
    }
    return g_cleanups;
}
#endif
