// Test module for nso2elf: classes with vtables, RTTI, exceptions,
// static constructors, data with pointers (relocations) and "imports"
// (unresolved symbols that end up in the PLT/GOT, as in a real NSO).
#include <cstdint>
#include <cstring>
#include <typeinfo>

extern "C" int nx_test_import_func(int);   // undefined: PLT entry + JUMP_SLOT
extern "C" int nx_test_import_data;        // undefined: GOT + GLOB_DAT

namespace tst {

class Base {
public:
    explicit Base(int id) : mId(id) {}
    virtual ~Base() = default;
    virtual int Calc(int x) const { return x + mId; }
    virtual const char* Name() const { return "Base"; }
    int Id() const { return mId; }

protected:
    int mId;
};

class Derived : public Base {
public:
    Derived(int id, float scale) : Base(id), mScale(scale) {}
    int Calc(int x) const override { return static_cast<int>(x * mScale) - mId; }
    const char* Name() const override { return "Derived"; }

private:
    float mScale;
};

class Other final : public Derived {
public:
    Other() : Derived(7, 2.5f) {}
    const char* Name() const override { return typeid(*this).name(); }
};

struct Table {
    const char* name;
    int (*fn)(int);
    const Base* obj;
};

}  // namespace tst

extern "C" {

__attribute__((noinline, visibility("default"))) int tst_square(int v) { return v * v; }
__attribute__((noinline, visibility("default"))) int tst_cube(int v) { return v * v * v; }
__attribute__((noinline, visibility("default"))) uint32_t tst_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= 16777619u;
    }
    return h;
}

}

namespace {
tst::Base gBase(1);
tst::Derived gDerived(2, 1.5f);
tst::Other gOther;
}  // namespace

// Data with pointers: they generate RELATIVE relocations in .data / .data.rel.ro
__attribute__((visibility("default"))) tst::Table gTable[] = {
    {"square", tst_square, &gBase},
    {"cube", tst_cube, &gDerived},
    {"other", nullptr, &gOther},
};

__attribute__((visibility("default"))) int gCounter = 42;
__attribute__((visibility("default"))) int gZeroed[64];  // .bss

__attribute__((noinline, visibility("default"))) int tst_dispatch(const tst::Base* b, int x) {
    if (const auto* d = dynamic_cast<const tst::Derived*>(b))
        return d->Calc(x) + 1;
    return b->Calc(x);
}

__attribute__((noinline, visibility("default"))) int tst_may_throw(int x) {
    try {
        if (x < 0)
            throw x;
        return x;
    } catch (int e) {
        return -e;
    }
}

__attribute__((noinline, visibility("default"))) int tst_use_imports(int x) {
    return nx_test_import_func(x) + nx_test_import_data;
}

int main(int argc, char** argv) {
    int acc = 0;
    for (const auto& t : gTable) {
        acc += tst_hash(t.name) & 0xff;
        if (t.fn)
            acc += t.fn(argc);
        acc += tst_dispatch(t.obj, argc);
        acc += std::strlen(t.obj->Name());
    }
    acc += tst_may_throw(-argc) + gCounter + gZeroed[argc & 63];
    if (argc > 100)
        acc += tst_use_imports(argc);
    return acc;
}
