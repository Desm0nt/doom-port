// [nonadreno] Just enough GLSL for C++ to compile xbg_bc_gpu.glsl on the host (bc_gpu_decode_test.cpp): integer/float vectors
// with component-wise operators, the bitfield built-ins with GLSL semantics, mix/equal/lessThan/greaterThanEqual, abs, min,
// max. Needs C++20 (parenthesised aggregate init of the decoder structs; defined shifts of negative ints like GLSL).
// Strict (06.11): what GLSL / SPIR-V leave undefined aborts here instead of getting the host's answer: a vector index outside the
// vector, bitfieldExtract with a negative offset / count or offset + count > 32. Shift counts of plain ints cannot be intercepted:
// build the test with -fsanitize=shift,bounds (the test's build line does) so a shift by >= 32 or < 0 aborts too.
#pragma once
#include <stdint.h>
#include <type_traits>
#include <assert.h>

typedef uint32_t uint;
template<class T, int N> struct gvec;
template<class T> struct gvec<T, 2> {
    union { struct { T x, y; }; T v[2]; };
    gvec() : v{} {}
    explicit gvec(T s) : v{s, s} {}
    template<class A, class B> gvec(A a, B b) : v{T(a), T(b)} {}
    template<class U> explicit gvec(const gvec<U, 2> &o) : v{T(o.v[0]), T(o.v[1])} {}
    T &operator[](int i) {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];} T operator[](int i) const {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];}
    T &operator[](uint i) {assert(i < sizeof v / sizeof *v); return v[i];} T operator[](uint i) const {assert(i < sizeof v / sizeof *v); return v[i];}
};
template<class T> struct gvec<T, 3> {
    union { struct { T x, y, z; }; T v[3]; };
    gvec() : v{} {}
    explicit gvec(T s) : v{s, s, s} {}
    template<class A, class B, class C> gvec(A a, B b, C c) : v{T(a), T(b), T(c)} {}
    template<class U> explicit gvec(const gvec<U, 3> &o) : v{T(o.v[0]), T(o.v[1]), T(o.v[2])} {}
    T &operator[](int i) {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];} T operator[](int i) const {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];}
};
template<class T> struct gvec<T, 4> {
    union { struct { T x, y, z, w; }; T v[4]; };
    gvec() : v{} {}
    explicit gvec(T s) : v{s, s, s, s} {}
    template<class A, class B, class C, class D> gvec(A a, B b, C c, D d) : v{T(a), T(b), T(c), T(d)} {}
    template<class U, class A> gvec(const gvec<U, 3> &o, A a) : v{T(o.v[0]), T(o.v[1]), T(o.v[2]), T(a)} {}
    template<class U, class A, class B> gvec(const gvec<U, 2> &o, A a, B b) : v{T(o.v[0]), T(o.v[1]), T(a), T(b)} {}
    template<class U> explicit gvec(const gvec<U, 4> &o) : v{T(o.v[0]), T(o.v[1]), T(o.v[2]), T(o.v[3])} {}
    T &operator[](int i) {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];} T operator[](int i) const {assert(i >= 0 && i < int(sizeof v / sizeof *v)); return v[i];}
    T &operator[](uint i) {assert(i < sizeof v / sizeof *v); return v[i];} T operator[](uint i) const {assert(i < sizeof v / sizeof *v); return v[i];}
};
typedef gvec<int, 2> ivec2; typedef gvec<int, 3> ivec3; typedef gvec<int, 4> ivec4;
typedef gvec<uint, 2> uvec2; typedef gvec<uint, 3> uvec3; typedef gvec<uint, 4> uvec4;
typedef gvec<bool, 3> bvec3;

#define XBG_SHIM_OP(op) \
template<class T, int N> gvec<T, N> operator op(const gvec<T, N> &a, const gvec<T, N> &b) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = T(a.v[i] op b.v[i]); return r;} \
template<class T, int N, class S, class = std::enable_if_t<std::is_arithmetic_v<S>>> gvec<T, N> operator op(const gvec<T, N> &a, S b) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = T(a.v[i] op T(b)); return r;} \
template<class T, int N, class S, class = std::enable_if_t<std::is_arithmetic_v<S>>> gvec<T, N> operator op(S a, const gvec<T, N> &b) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = T(T(a) op b.v[i]); return r;} \
template<class T, int N> gvec<T, N> &operator op##=(gvec<T, N> &a, const gvec<T, N> &b) {a = a op b; return a;}
XBG_SHIM_OP(+) XBG_SHIM_OP(-) XBG_SHIM_OP(*) XBG_SHIM_OP(/) XBG_SHIM_OP(&) XBG_SHIM_OP(|) XBG_SHIM_OP(<<) XBG_SHIM_OP(>>)
#undef XBG_SHIM_OP
template<class T, int N> gvec<T, N> operator-(const gvec<T, N> &a) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = T(-a.v[i]); return r;}

static inline void xbg_bfe_check(int off, int bits) {assert(off >= 0 && bits >= 0 && off + bits <= 32 && "bitfieldExtract outside the 32 bits: undefined in GLSL");}
static inline uint bitfieldExtract(uint v, int off, int bits) {xbg_bfe_check(off, bits); return bits == 0 ? 0u : bits == 32 ? v >> off : (v >> off) & ((1u << bits) - 1u);}
static inline int bitfieldExtract(int v, int off, int bits) {xbg_bfe_check(off, bits); return bits == 0 ? 0 : int(uint(v) << (32 - off - bits)) >> (32 - bits);}
template<class T, int N> gvec<T, N> bitfieldExtract(const gvec<T, N> &a, int off, int bits) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = bitfieldExtract(a.v[i], off, bits); return r;}
static inline uint bitfieldReverse(uint v) {uint r = 0; for(int i = 0; i < 32; i++) if(v >> i & 1u) r |= 1u << (31 - i); return r;}
static inline int findLSB(uint v) {return v ? __builtin_ctz(v) : -1;}
static inline int findMSB(uint v) {return v ? 31 - __builtin_clz(v) : -1;}
static inline int max(int a, int b) {return a > b ? a : b;}
static inline int clamp(int x, int lo, int hi) {return x < lo ? lo : x > hi ? hi : x;}
static inline uint max(uint a, uint b) {return a > b ? a : b;}
static inline uint min(uint a, uint b) {return a < b ? a : b;}
static inline float max(float a, float b) {return a > b ? a : b;}
template<int N> gvec<int, N> abs(const gvec<int, N> &a) {gvec<int, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] < 0 ? -a.v[i] : a.v[i]; return r;}
template<class T, int N> gvec<T, N> mix(const gvec<T, N> &a, const gvec<T, N> &b, const gvec<bool, N> &s) {gvec<T, N> r; for(int i = 0; i < N; i++) r.v[i] = s.v[i] ? b.v[i] : a.v[i]; return r;}
template<class T, int N> gvec<bool, N> equal(const gvec<T, N> &a, const gvec<T, N> &b) {gvec<bool, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] == b.v[i]; return r;}
template<class T, int N> gvec<bool, N> lessThan(const gvec<T, N> &a, const gvec<T, N> &b) {gvec<bool, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] < b.v[i]; return r;}
template<class T, int N> gvec<bool, N> greaterThanEqual(const gvec<T, N> &a, const gvec<T, N> &b) {gvec<bool, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] >= b.v[i]; return r;}
