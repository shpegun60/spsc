/*
 * spsc_cacheline.hpp
 *
 * Created on: 30 Nov. 2025
 *      Author: Shpegun60
 *
 * Cross-platform cache-line size deduction for MCUs and desktops.
 *
 * Exposes:
 *   - Macro  SPSC_CACHELINE_BYTES   : detected / forced cache-line size in bytes
 *   - Macro  SPSC_ALIGNED(N)        : portable alignment attribute helper
 *   - C++    spsc::hw::cacheline_bytes : constexpr wrapper for SPSC_CACHELINE_BYTES
 *
 * Features:
 *   - Purely macro-based; zero runtime overhead, zero library dependencies.
 *   - Safe user overrides via:
 *       -DSPSC_FORCE_CACHELINE=64
 *   - Conservative 32-byte floor for MCU / no-cache targets to avoid under-alignment
 *     for DMA / coherency-sensitive structures.
 *
 * Typical usage:
 *
 *   // Align a type to the cache-line size:
 *   struct SPSC_ALIGNED(SPSC_CACHELINE_BYTES) MyAlignedStruct {
 *       ...
 *   };
 *
 * Design notes:
 *   - Detection prefers:
 *       1) Explicit user overrides
 *       2) Exact platforms with well-known cache-line sizes
 *       3) Family-level heuristics with conservative defaults
 *       4) Fallback (64 bytes) if nothing matches
 *   - Qt awareness is optional and only used if <QtCore/qglobal.h> is present.
 *   - ARM M-profile is explicitly excluded from A-profile branches to avoid
 *     mis-detection via generic __arm__ / __aarch64__ macros.
 *   - Power-of-two and minimum-size checks are enforced at preprocess time.
 */

#ifndef SPSC_CACHELINE_DETECT_HPP_
#define SPSC_CACHELINE_DETECT_HPP_

#include "spsc_config.hpp"

/* ────────────────────────────────────────────────────────────────────────────
 * User override: SPSC_FORCE_CACHELINE
 *
 * Example:
 *   -DSPSC_FORCE_CACHELINE=128
 *
 * We wrap in (0u + ...) to force the preprocessor into a numeric literal
 * context suitable for arithmetic (bit tests, comparisons, etc).
 * ──────────────────────────────────────────────────────────────────────────── */
#if defined(SPSC_FORCE_CACHELINE)
#  define SPSC__FORCED_CL_BYTES (0u + SPSC_FORCE_CACHELINE)
#endif

/* Minimal reasonable alignment when there is no explicit data cache.
 *
 * 32 bytes is a pragmatic floor:
 *   - Good enough for many DMA alignment requirements on MCUs.
 *   - Harmlessly conservative on small cores without caches.
 */
#ifndef SPSC_CACHELINE_MIN
#  define SPSC_CACHELINE_MIN 32u
#endif /* SPSC_CACHELINE_MIN */

#if ((SPSC_CACHELINE_MIN & (SPSC_CACHELINE_MIN - 1u)) != 0)
#  error "SPSC_CACHELINE_MIN must be a power-of-two"
#endif


/* ────────────────────────────────────────────────────────────────────────────
 * Optional Qt awareness
 *
 * If Qt is available, we include <QtCore/qglobal.h> to use Q_PROCESSOR_*
 * macros for more precise desktop/host detection.
 *
 * For MCU / bare-metal builds:
 *   - Qt is usually absent
 *   - This block is inert
 * ──────────────────────────────────────────────────────────────────────────── */
#ifndef SPSC__HAVE_QT
#  define SPSC__HAVE_QT 0
#endif /* SPSC__HAVE_QT */

#if (defined(QT_CORE_LIB) || defined(QT_VERSION)) && !defined(SPSC_NO_QT_CACHELINE)
#  include <QtCore/qglobal.h>
#  undef  SPSC__HAVE_QT
#  define SPSC__HAVE_QT 1
#endif /* SPSC__HAVE_QT */

/* ────────────────────────────────────────────────────────────────────────────
 * Primary detection: SPSC_CACHELINE_BYTES
 *
 * Order of preference:
 *   1) Explicit override (SPSC_FORCE_CACHELINE)
 *   2) Exact platforms with well-known sizes
 *   3) Family-level heuristics with conservative defaults
 *   4) Generic desktop/server fallback (64 bytes)
 *
 * Notes:
 *   - Apple ARM64: 128B L1D lines on Apple Silicon in practice.
 *   - x86/x64 desktops: 64B lines almost universally.
 *   - Many MCUs have no data cache; we keep a 32B floor to avoid under-alignment.
 *   - ARM A-profile (non-Apple): 64B is a safe and common default.
 * ──────────────────────────────────────────────────────────────────────────── */
#ifndef SPSC_CACHELINE_BYTES

/* 1) Explicit override first */
# if defined(SPSC__FORCED_CL_BYTES)

#   define SPSC_CACHELINE_BYTES SPSC__FORCED_CL_BYTES

/* 2) Apple ARM64: practical 128B L1D lines on Apple Silicon */
# elif defined(__APPLE__) && defined(__aarch64__)

#   define SPSC_CACHELINE_BYTES 128u

/* 3) Qt-hosted desktop targets (using Q_PROCESSOR_* macros) */
# elif (SPSC__HAVE_QT) && (defined(Q_PROCESSOR_X86_64) || defined(Q_PROCESSOR_X86))

#   define SPSC_CACHELINE_BYTES 64u

# elif (SPSC__HAVE_QT) && defined(Q_PROCESSOR_ARM_64)

#   define SPSC_CACHELINE_BYTES 64u

# elif (SPSC__HAVE_QT) && defined(Q_PROCESSOR_ARM)
/* Qt on ARM usually means A-profile Linux/Android; 64B is standard there. */
#   define SPSC_CACHELINE_BYTES 64u

/* 4) Specific MCU families: prefer conservative 32B even when cache-less.
 *
 * This is deliberately cautious: MCU data paths (DMA, coherency, bus masters)
 * often work better with "too much" alignment than with too little.
 */
# elif defined(STM32H7) || defined(STM32H7xx) || defined(STM32H743xx) || defined(STM32H753xx) || \
		defined(STM32F7xx) || defined(STM32F4xx) || defined(STM32H5xx)  || defined(STM32U5xx)  || \
		defined(STM32L4xx) || defined(STM32G4xx) || defined(STM32F3xx)  || defined(STM32F1xx)  || \
		defined(STM32F0xx)

#   define SPSC_CACHELINE_BYTES 32u

/* Generic Cortex-M (M7/M33/M4/M3/M23/M0+): keep 32B safe pad */
# elif defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__) || \
		defined(__ARM_ARCH_7EM__)     || defined(__ARM_ARCH_7M__)        || \
		defined(__ARM_ARCH_6M__)

#   define SPSC_CACHELINE_BYTES 32u

/* ESP32 / Xtensa: 32B is a pragmatic choice used in many SDKs and examples. */
# elif defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32) || defined(__XTENSA__)

#   define SPSC_CACHELINE_BYTES 32u

/* AVR / MSP430 / other tiny MCUs: no data cache, but give some pad anyway. */
# elif defined(__AVR__) || defined(__MSP430__)

#   define SPSC_CACHELINE_BYTES 32u

/* PIC32 / MIPS: many parts use 32B lines; choose 32B conservatively. */
# elif defined(__mips__) || defined(__MIPS__)

#   define SPSC_CACHELINE_BYTES 32u

/* RISC-V:
 *   - If building on a full OS (Linux / Windows / Apple): assume 64B desktop.
 *   - Otherwise (bare-metal / small RTOS): pick 32B.
 */
# elif defined(__riscv)

#   if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#     define SPSC_CACHELINE_BYTES 64u
#   else
#     define SPSC_CACHELINE_BYTES 32u
#   endif

/* ARM A-profile (non-Apple):
 *   - Common case: 64B lines.
 *   - We explicitly exclude M-profile via !__ARM_ARCH_*M* guards to prevent
 *     mis-detection from generic __arm__ / __aarch64__ defines.
 */
# elif (defined(__aarch64__) || defined(_M_ARM64)) || \
		((defined(__arm__) || defined(_M_ARM)) && \
				!defined(__ARM_ARCH_8M_MAIN__) && !defined(__ARM_ARCH_8_1M_MAIN__) && \
				!defined(__ARM_ARCH_7EM__)     && !defined(__ARM_ARCH_7M__)        && \
				!defined(__ARM_ARCH_6M__))

#   define SPSC_CACHELINE_BYTES 64u

/* x86/x64 desktops and servers: 64B is effectively universal. */
# elif defined(__x86_64__) || defined(_M_X64) || \
		defined(__i386__)   || defined(_M_IX86)

#   define SPSC_CACHELINE_BYTES 64u

/* PowerPC:
 * Many ppc64 parts use 128B lines; using 128B for both 32/64-bit here.
 */
# elif defined(__powerpc64__) || defined(__ppc64__) || \
		defined(__powerpc__)   || defined(__ppc__)

#   define SPSC_CACHELINE_BYTES 128u

/* Fallback: generic 64B cache line for unknown desktop/server targets. */
# else

#   define SPSC_CACHELINE_BYTES 64u

# endif
#endif /* !SPSC_CACHELINE_BYTES */

/* ────────────────────────────────────────────────────────────────────────────
 * Sanity checks: enforce minimum and power-of-two property
 *
 * 1) If user/platform provided a value smaller than SPSC_CACHELINE_MIN,
 *    clamp it to SPSC_CACHELINE_MIN.
 * 2) Enforce power-of-two: any misconfiguration is caught as a build error.
 * ──────────────────────────────────────────────────────────────────────────── */
#if (SPSC_CACHELINE_BYTES < SPSC_CACHELINE_MIN)
#  undef  SPSC_CACHELINE_BYTES
#  define SPSC_CACHELINE_BYTES SPSC_CACHELINE_MIN
#endif

#if ((SPSC_CACHELINE_BYTES & (SPSC_CACHELINE_BYTES - 1u)) != 0)
#  error "SPSC_CACHELINE_BYTES must be a power-of-two"
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Alignment attribute helper
 *
 * SPSC_ALIGNED(n) can be used to align types / variables to N bytes:
 *
 *   struct SPSC_ALIGNED(SPSC_CACHELINE_BYTES) Node {
 *       ...
 *   };
 *
 * MSVC       : __declspec(align(n))
 * GCC/Clang  : __attribute__((aligned(n)))
 * IAR        : modern versions accept GCC-style attributes here; if a target
 *              requires #pragma-based alignment instead, handle it at the call
 *              site or inject an alternative definition via compile flags.
 * ──────────────────────────────────────────────────────────────────────────── */
#if defined(_MSC_VER)
#  define SPSC_ALIGNED(n) __declspec(align(n))
#elif defined(__ICCARM__) || defined(__IAR_SYSTEMS_ICC__)
#  define SPSC_ALIGNED(n) __attribute__((aligned(n)))
#else
#  define SPSC_ALIGNED(n) __attribute__((aligned(n)))
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * C++ convenience: spsc::hw::cacheline_bytes
 *
 * This mirrors SPSC_CACHELINE_BYTES as a constexpr value for templates and
 * static_asserts, without adding any runtime cost.
 * ──────────────────────────────────────────────────────────────────────────── */
#ifdef __cplusplus
namespace spsc::hw {
	static constexpr unsigned cacheline_bytes = SPSC_CACHELINE_BYTES;
}
#endif /* __cplusplus */

#endif /* SPSC_CACHELINE_DETECT_HPP_ */
