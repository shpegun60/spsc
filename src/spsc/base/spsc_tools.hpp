/*
 * spsc_tools.hpp
 *
 *  Created on: 30 Nov. 2025
 *      Author: Shpegun60
 *
 * Tiny portability helpers for inlining attributes across C/C++ compilers.
 * - Zero dependencies, header-only, safe for inclusion from multiple TUs.
 * - Keeps C ODR safe by using 'static inline' where appropriate.
 * - Provides a single token for "force inline" and "no inline".
 *
 * Usage:
 *   static RB_FORCEINLINE int add(int a, int b) { return a + b; }
 *   RB_NOINLINE void hot_boundary(void) {  ...  }
 *
 * Notes:
 * - For GCC/Clang, 'always_inline' is honored only if the function body
 *   is visible. Keep the definition in the header if you expect inlining.
 * - For IAR, true force-inlining requires a pragma; we expose a helper
 *   RB_IAR_FORCE_INLINE() that you can place immediately before the function.
 */

#ifndef SPSC_TOOLS_HPP_
#define SPSC_TOOLS_HPP_

#include "spsc_config.hpp"

// ============================================================================
// ASSERT Macro
// ============================================================================
#ifndef SPSC_ASSERT
#  define SPSC_ASSERT(x)
#endif /* SPSC_ASSERT */

/* ---------------------------------------------------------------------------
 * RB_INLINE: header-safe inline for both C and C++
 * - In C, 'static inline' avoids multiple definition/linker issues.
 * - In C++, plain 'inline' is typically sufficient.
 * ------------------------------------------------------------------------- */
#ifndef RB_INLINE
#  ifdef __cplusplus
#    define RB_INLINE inline
#  else
#    if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#      define RB_INLINE static inline
#    else
     /* Very old C compilers: no inline support, fall back to 'static'. */
#      define RB_INLINE static
#    endif
#  endif
#endif /* RB_INLINE */

/* ---------------------------------------------------------------------------
 * RB_FORCEINLINE: "strong" inlining hint for headers
 * - One macro that maps to the compiler's force-inline attribute.
 * - Always pair it with 'static' at call site when used in C headers:
 *     static RB_FORCEINLINE int f(int x) { return x; }
 * - For IAR, define a helper pragma macro that can be placed before the
 *   next function if you truly need to force it.
 * ------------------------------------------------------------------------- */
#ifndef RB_FORCEINLINE
  /* MSVC or clang-cl */
#  if defined(_MSC_VER)
#    define RB_FORCEINLINE __forceinline
  /* Clang/GCC style (Clang also defines __GNUC__, so check __clang__ first) */
#  elif defined(__clang__) || defined(__GNUC__)
#    define RB_FORCEINLINE inline __attribute__((always_inline))
  /* IAR: cannot truly force without pragma; provide 'inline' and a helper. */
#  elif defined(__ICCARM__) || defined(__IAR_SYSTEMS_ICC__)
#    define RB_FORCEINLINE inline
#    ifndef RB_IAR_FORCE_INLINE
       /* Put this macro immediately before the function to request forcing. */
#      define RB_IAR_FORCE_INLINE() _Pragma("inline=forced")
#    endif
  /* ARMCC (armcc/armcc5), non-clang front-end */
#  elif defined(__ARMCC_VERSION) && !defined(__clang__)
#    define RB_FORCEINLINE __forceinline
  /* Fallback: at least hint 'inline' */
#  else
#    define RB_FORCEINLINE inline
#  endif
#endif /* RB_FORCEINLINE */

/* ---------------------------------------------------------------------------
 * RB_NOINLINE: prevent inlining
 * - Useful for profiling, ABI fences, or to keep symbol boundaries visible.
 * - On IAR the pragma applies to the next function only; place the macro
 *   directly before the function definition.
 * ------------------------------------------------------------------------- */
#ifndef RB_NOINLINE
#  if defined(_MSC_VER)
#    define RB_NOINLINE __declspec(noinline)
#  elif defined(__clang__) || defined(__GNUC__)
#    define RB_NOINLINE __attribute__((noinline))
#  elif defined(__ICCARM__) || defined(__IAR_SYSTEMS_ICC__)
     /* Applies to the next function definition only. */
#    define RB_NOINLINE _Pragma("inline=never")
#  else
     /* Unknown toolchain: degrade gracefully. */
#    define RB_NOINLINE
#  endif
#endif /* RB_NOINLINE */

/* ---------------------------------------------------------------------------
 * Local fallbacks for branch prediction hints.
 * Separate guards prevent losing RB_UNLIKELY if RB_LIKELY is predefined.
 * ------------------------------------------------------------------------- */
#ifndef RB_LIKELY
#  if defined(__clang__) || defined(__GNUC__)
#    define RB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  else
#    define RB_LIKELY(x)   (x)
#  endif
#endif /* RB_LIKELY */

#ifndef RB_UNLIKELY
#  if defined(__clang__) || defined(__GNUC__)
#    define RB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  else
#    define RB_UNLIKELY(x) (x)
#  endif
#endif /* RB_UNLIKELY */

// ============================================================================
// Exceptions helpers
// ============================================================================

// Optional sanity check: if user forces 1 but compiler clearly has no exceptions,
// fail at compile-time instead of pretending everything is fine.
#if SPSC_ENABLE_EXCEPTIONS
#  if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && \
		!(defined(_MSC_VER) && defined(_CPPUNWIND))
#    error "SPSC_ENABLE_EXCEPTIONS=1 but compiler appears to have exceptions disabled"
#  endif
#endif /* SPSC_ENABLE_EXCEPTIONS */

#if !defined(SPSC_TRY)
#  if SPSC_ENABLE_EXCEPTIONS
#    define SPSC_TRY       try
#    define SPSC_CATCH_ALL catch (...)
#    define SPSC_RETHROW   throw
#  else
#    if defined(__cpp_if_constexpr) && __cpp_if_constexpr >= 201606
#      define SPSC_TRY
#      define SPSC_CATCH_ALL if constexpr (false)
#    else
#      define SPSC_TRY
#      define SPSC_CATCH_ALL if (false)
#    endif
#    define SPSC_RETHROW
#  endif
#endif /* SPSC_TRY */

// ============================================================================
// C++20 SPAN
// ============================================================================
#if defined(__has_include)
#  if __has_include(<span>) && (__cplusplus >= 202002L)
#    include <span>
#    define SPSC_HAS_SPAN 1
#  else
#    define SPSC_HAS_SPAN 0
#  endif
#else
#  define SPSC_HAS_SPAN 0
#endif/* SPSC_HAS_SPAN */


#endif /* SPSC_TOOLS_HPP_ */
