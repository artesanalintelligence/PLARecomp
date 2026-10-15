// Test hierarchy for rtti.py / eh_frame.py.
// Covers single, multiple and virtual (diamond) inheritance, abstract classes,
// namespaces, templates and a class with no bases.
namespace game {

struct Base {
    virtual ~Base() {}
    virtual int id() const { return 1; }
    virtual void update(float) {}
    int b = 0;
};

struct Derived : Base {  // single inheritance -> __si_class_type_info
    int id() const override { return 2; }
    virtual void extra() {}
    int d = 0;
};

struct Abstract {
    virtual ~Abstract() {}
    virtual void pure() = 0;  // entry to __cxa_pure_virtual
};

struct Mixin {
    virtual ~Mixin() {}
    virtual int mix() { return 3; }
    int m = 0;
};

struct Multi : Derived, Mixin {  // multiple inheritance -> __vmi_class_type_info
    int id() const override { return 4; }
    int mix() override { return 5; }
};

struct VBase {
    virtual ~VBase() {}
    virtual int vb() { return 6; }
    int v = 0;
};
struct Left : virtual VBase { int vb() override { return 7; } int l = 0; };
struct Right : virtual VBase { virtual int r() { return 8; } int rr = 0; };
struct Diamond : Left, Right {  // diamond with virtual base
    int vb() override { return 9; }
    int r() override { return 10; }
};

struct Impl : Abstract {
    void pure() override {}
};

template <typename T, int N>
struct Holder : Base {
    int id() const override { return N; }
    T value{};
};

}  // namespace game

namespace {
struct LocalOnly {  // class with internal linkage
    virtual ~LocalOnly() {}
    virtual int f() { return 11; }
};
}  // namespace

// Exported so the linker keeps everything.
extern "C" game::Base* make_base(int k) {
    switch (k) {
    case 0: return new game::Base;
    case 1: return new game::Derived;
    case 2: return new game::Multi;
    case 3: return new game::Holder<int, 3>;
    default: return new game::Holder<float, 4>;
    }
}
extern "C" game::VBase* make_vbase(int k) {
    if (k == 0) return new game::Left;
    if (k == 1) return new game::Right;
    return new game::Diamond;
}
extern "C" game::Abstract* make_abstract() { return new game::Impl; }
extern "C" game::Mixin* make_mixin() { return new game::Multi; }
extern "C" void* make_local() { return new LocalOnly; }
extern "C" int call_all(game::Base* b, game::VBase* v) {
#if __GXX_RTTI
    return b->id() + v->vb() + (dynamic_cast<game::Derived*>(b) ? 1 : 0);
#else
    return b->id() + v->vb();
#endif
}
