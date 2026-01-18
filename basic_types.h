/*
 * basic_types.h — Standard platform-independent type aliases
 *
 * This file defines a full set of integer, floating-point, and platform-native
 * types for both C and C++ in a consistent and portable way. Use them to write
 * clear, architecture-agnostic code.
 *
 * ────────────────────────────────────────────────────────────────────────────────
 *  Category      │ Purpose                         │ Example types
 * ───────────────┼─────────────────────────────────┼───────────────────────────────
 *  Exact-width   │ Fixed bit size                  │ u8,  i32,  s32,  u64
 *  Least-width   │ Smallest type ≥ N bits          │ u8l, i16l, u32l
 *  Fast          │ Fastest type ≥ N bits           │ u8f, i32f, u64f
 *  Native        │ Pointer-sized (register proxy)  │ reg, sreg
 *  Floating      │ Standard float/double/long dbl  │ f32, f64, f128
 *  Size-friendly │ API ergonomics for sizes        │ usize (size_t), isize (ptrdiff_t)
 *  Char family   │ Char + explicit signedness      │ c8, sc8, uc8
 *  Misc          │ Other types                     │ b (bool/_Bool), uni (void)
 *  Volatile      │ MMIO-safe mirrors               │ vu32, vi16, vreg, vf32
 * ────────────────────────────────────────────────────────────────────────────────
 *
 * Type naming convention:
 *     - Prefixes:
 *         u  = unsigned integer
 *         i  = signed integer
 *         s  = synonym for signed integer (alias of iN, e.g. s32 == i32)
 *         vu = volatile unsigned integer
 *         vi = volatile signed integer
 *
 *     - Width:
 *         8, 16, 32, 64  → bit width
 *
 *     - Suffixes:
 *         (none) = exact-width
 *         l      = least-width (smallest type with at least N bits)
 *         f      = fast-width  (fastest type with at least N bits)
 *
 * Examples:
 *     - u16   → exactly 16-bit unsigned integer
 *     - i32   → exactly 32-bit signed integer
 *     - s32   → alias for i32 (signed 32-bit integer)
 *     - u32l  → at least 32-bit unsigned integer (smallest that fits)
 *     - u8f   → fastest unsigned type ≥ 8 bits
 *     - reg   → pointer-sized unsigned integer (native word size)
 *     - usize → alias for size_t, convenient in APIs
 *     - c8/sc8/uc8 → char with implementation-defined vs explicit signedness
 *     - vu32  → exactly 32-bit volatile unsigned integer
 *
 * Platform assumptions:
 *     - 8-bit bytes.
 *     - Standard 32/64-bit integer layout.
 *     - 32-bit float and 64-bit double. These are enforced via static asserts
 *       so that unsupported targets fail at compile time.
 *
 * This file also provides size checks to fail early on incompatible platforms.
 */

#ifndef BASIC_TYPES_H_
#define BASIC_TYPES_H_

/* ------------------------------- Includes ----------------------------------- */
#ifdef __cplusplus
# include <cstdint>   /* integer types */
# include <cstddef>   /* size_t, ptrdiff_t */
#else
# include <stdint.h>
# include <stddef.h>
# include <stdbool.h>
#endif /* __cplusplus */

/* --------------------------------------------------------------------------
 * Compile-time assert abstraction (C++ / C11 / C99 fallback)
 * -------------------------------------------------------------------------- */
#if defined(__cplusplus)
# define BT_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
# define BT_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
# define BT_CAT_(a,b) a##b
# define BT_CAT(a,b)  BT_CAT_(a,b)
# define BT_STATIC_ASSERT(cond, msg) \
    typedef char BT_CAT(static_assert_failed_at_line_,__LINE__)[(cond)?1:-1]
#endif /* BT_STATIC_ASSERT SELECTION */

/* ------------------------------ Type aliases -------------------------------- */

#ifdef __cplusplus /* ======================== C++ ========================= */

/* Exact-width integer types (guaranteed size) */
using u8  = std::uint8_t;   using i8  = std::int8_t;   using s8  = i8;
using u16 = std::uint16_t;  using i16 = std::int16_t;  using s16 = i16;
using u32 = std::uint32_t;  using i32 = std::int32_t;  using s32 = i32;
using u64 = std::uint64_t;  using i64 = std::int64_t;  using s64 = i64;

/* Least-width integer types (≥N bits, smallest possible) */
using u8l  = std::uint_least8_t;   using i8l  = std::int_least8_t;
using u16l = std::uint_least16_t;  using i16l = std::int_least16_t;
using u32l = std::uint_least32_t;  using i32l = std::int_least32_t;
using u64l = std::uint_least64_t;  using i64l = std::int_least64_t;

/* Fast integer types (≥N bits, fastest on platform) */
using u8f  = std::uint_fast8_t;    using i8f  = std::int_fast8_t;
using u16f = std::uint_fast16_t;   using i16f = std::int_fast16_t;
using u32f = std::uint_fast32_t;   using i32f = std::int_fast32_t;
using u64f = std::uint_fast64_t;   using i64f = std::int_fast64_t;

/* Native register-size types (match pointer size) */
using reg  = std::size_t;      /* unsigned native word (for indices/capacities) */
using sreg = std::ptrdiff_t;   /* signed native word (for differences) */

/* Pointer-sized integer aliases (for pointer → integer round-trip) */
using reg_ptr  = std::uintptr_t;
using sreg_ptr = std::intptr_t;

/* Size-friendly aliases (API ergonomics) */
using usize = std::size_t;
using isize = std::ptrdiff_t;

/* Floating-point types */
using f32  = float;
using f64  = double;
using f128 = long double; /* platform-dependent */

/* Misc / char family */
using c8  = char;          /* implementation-defined signedness */
using sc8 = signed char;   /* explicit signed 8-bit char */
using uc8 = unsigned char; /* explicit unsigned 8-bit char */
using b   = bool;
using uni = void;

#else /* ========================= C (C99/C11) ========================= */

/* Exact-width integer types (guaranteed size) */
typedef uint8_t  u8;   typedef int8_t  i8;   typedef i8  s8;
typedef uint16_t u16;  typedef int16_t i16;  typedef i16 s16;
typedef uint32_t u32;  typedef int32_t i32;  typedef i32 s32;
typedef uint64_t u64;  typedef int64_t i64;  typedef i64 s64;

/* Least-width integer types (≥N bits, smallest possible) */
typedef uint_least8_t  u8l;   typedef int_least8_t  i8l;
typedef uint_least16_t u16l;  typedef int_least16_t i16l;
typedef uint_least32_t u32l;  typedef int_least32_t i32l;
typedef uint_least64_t u64l;  typedef int_least64_t i64l;

/* Fast integer types (≥N bits, fastest on platform) */
typedef uint_fast8_t  u8f;    typedef int_fast8_t  i8f;
typedef uint_fast16_t u16f;   typedef int_fast16_t i16f;
typedef uint_fast32_t u32f;   typedef int_fast32_t i32f;
typedef uint_fast64_t u64f;   typedef int_fast64_t i64f;

/* Native register-size types (match pointer size) */
typedef size_t    reg;   /* unsigned native word (for indices/capacities) */
typedef ptrdiff_t sreg;  /* signed native word (for differences) */

/* Pointer-sized integer aliases (for pointer → integer round-trip) */
typedef uintptr_t reg_ptr;
typedef intptr_t  sreg_ptr;

/* Size-friendly aliases (API ergonomics) */
typedef size_t    usize;
typedef ptrdiff_t isize;

/* Floating-point types */
typedef float       f32;
typedef double      f64;
typedef long double f128; /* platform-dependent */

/* Misc / char family */
typedef char           c8;   /* implementation-defined signedness */
typedef signed   char  sc8;  /* explicit signed 8-bit char */
typedef unsigned char  uc8;  /* explicit unsigned 8-bit char */
typedef _Bool          b;
typedef void           uni;

#endif /* __cplusplus */

/* --------------------------- Volatile mirrors ------------------------------- */
/* Volatile aliases mirror the non-volatile ones: v + <alias>.
 * Use for memory-mapped I/O or data that must not be optimized away.
 */

#ifdef __cplusplus  /* ======================== C++ ========================= */
using vu8  = volatile u8;    using vi8  = volatile i8;
using vu16 = volatile u16;   using vi16 = volatile i16;
using vu32 = volatile u32;   using vi32 = volatile i32;
using vu64 = volatile u64;   using vi64 = volatile i64;

using vu8l  = volatile u8l;  using vi8l  = volatile i8l;
using vu16l = volatile u16l; using vi16l = volatile i16l;
using vu32l = volatile u32l; using vi32l = volatile i32l;
using vu64l = volatile u64l; using vi64l = volatile i64l;

using vu8f  = volatile u8f;  using vi8f  = volatile i8f;
using vu16f = volatile u16f; using vi16f = volatile i16f;
using vu32f = volatile u32f; using vi32f = volatile i32f;
using vu64f = volatile u64f; using vi64f = volatile i64f;

/* Native/pointer-sized and size-friendly */
using vreg   = volatile reg;
using vsreg  = volatile sreg;
using vusize = volatile usize;
using visize = volatile isize;

/* Floating-point */
using vf32  = volatile f32;
using vf64  = volatile f64;
using vf128 = volatile f128;

/* Char family */
using vc8  = volatile c8;
using vsc8 = volatile sc8;
using vuc8 = volatile uc8;

#else             /* ========================= C (C99/C11) ================== */
typedef volatile u8   vu8;   typedef volatile i8   vi8;
typedef volatile u16  vu16;  typedef volatile i16  vi16;
typedef volatile u32  vu32;  typedef volatile i32  vi32;
typedef volatile u64  vu64;  typedef volatile i64  vi64;

typedef volatile u8l  vu8l;  typedef volatile i8l  vi8l;
typedef volatile u16l vu16l; typedef volatile i16l vi16l;
typedef volatile u32l vu32l; typedef volatile i32l vi32l;
typedef volatile u64l vu64l; typedef volatile i64l vi64l;

typedef volatile u8f  vu8f;  typedef volatile i8f  vi8f;
typedef volatile u16f vu16f; typedef volatile i16f vi16f;
typedef volatile u32f vu32f; typedef volatile i32f vi32f;
typedef volatile u64f vu64f; typedef volatile i64f vi64f;

/* Native/pointer-sized and size-friendly */
typedef volatile reg   vreg;
typedef volatile sreg  vsreg;
typedef volatile usize vusize;
typedef volatile isize visize;

/* Floating-point */
typedef volatile f32 vf32;
typedef volatile f64 vf64;
typedef volatile f128 vf128;

/* Char family */
typedef volatile c8  vc8;
typedef volatile sc8 vsc8;
typedef volatile uc8 vuc8;
#endif /* __cplusplus */

/* ------------------------------ Sanity checks ------------------------------- */
/* Exact-width integers must be exact */
BT_STATIC_ASSERT(sizeof(u8)  == 1, "u8 must be 1 byte");
BT_STATIC_ASSERT(sizeof(u16) == 2, "u16 must be 2 bytes");
BT_STATIC_ASSERT(sizeof(u32) == 4, "u32 must be 4 bytes");
BT_STATIC_ASSERT(sizeof(u64) == 8, "u64 must be 8 bytes");
BT_STATIC_ASSERT(sizeof(i8)  == 1, "i8 must be 1 byte");
BT_STATIC_ASSERT(sizeof(i16) == 2, "i16 must be 2 bytes");
BT_STATIC_ASSERT(sizeof(i32) == 4, "i32 must be 4 bytes");
BT_STATIC_ASSERT(sizeof(i64) == 8, "i64 must be 8 bytes");

/* Least/fast are lower bounds */
BT_STATIC_ASSERT(sizeof(u8l)  >= 1, "u8l >= 1");
BT_STATIC_ASSERT(sizeof(u16l) >= 2, "u16l >= 2");
BT_STATIC_ASSERT(sizeof(u32l) >= 4, "u32l >= 4");
BT_STATIC_ASSERT(sizeof(u64l) >= 8, "u64l >= 8");
BT_STATIC_ASSERT(sizeof(i8l)  >= 1, "i8l >= 1");
BT_STATIC_ASSERT(sizeof(i16l) >= 2, "i16l >= 2");
BT_STATIC_ASSERT(sizeof(i32l) >= 4, "i32l >= 4");
BT_STATIC_ASSERT(sizeof(i64l) >= 8, "i64l >= 8");

BT_STATIC_ASSERT(sizeof(u8f)  >= 1, "u8f >= 1");
BT_STATIC_ASSERT(sizeof(u16f) >= 2, "u16f >= 2");
BT_STATIC_ASSERT(sizeof(u32f) >= 4, "u32f >= 4");
BT_STATIC_ASSERT(sizeof(u64f) >= 8, "u64f >= 8");
BT_STATIC_ASSERT(sizeof(i8f)  >= 1, "i8f >= 1");
BT_STATIC_ASSERT(sizeof(i16f) >= 2, "i16f >= 2");
BT_STATIC_ASSERT(sizeof(i32f) >= 4, "i32f >= 4");
BT_STATIC_ASSERT(sizeof(i64f) >= 8, "i64f >= 8");

/* Pointer-sized aliases must match pointer size */
BT_STATIC_ASSERT(sizeof(reg)  == sizeof(void*), "reg must match pointer size");
BT_STATIC_ASSERT(sizeof(sreg) == sizeof(void*), "sreg must match pointer size");

/* Floats: f32/f64 are assumed to be 4/8 bytes on supported targets */
BT_STATIC_ASSERT(sizeof(f32) == 4, "float must be 4 bytes");
BT_STATIC_ASSERT(sizeof(f64) == 8, "double must be 8 bytes on this target");

#endif /* BASIC_TYPES_H_ */
