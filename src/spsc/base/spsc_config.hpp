/*
 * spsc_config.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPSC_CONFIG_HPP_
#define SPSC_CONFIG_HPP_

/*
 * SPSCbase settings
 * Build toggles:
 *   - SPSC_ENABLE_SHADOW_INDICES (default: 1)
 *       0 -> disable shadows entirely
 *       1 -> enable shadows when policy counter backend is atomic-backed (and width allows)
 *
 *   - SPSC_SHADOW_ALLOW_32BIT (default: 0)
 *       0 -> shadows only on reg width >= 64
 *       1 -> allow shadows even on 32-bit reg (useful on some RTOS setups)
 *
 *   - SPSC_SHADOW_REFRESH_HEURISTIC (default: 0)
 *       0 -> no extra refresh in write_size()/read_size()
 *       1 -> refresh once when shadow suggests we're close to boundary (avoid under-estimation)
 *
 *   - SPSC_SHADOW_REFRESH_FRAC_SHIFT (default: 2)
 *       Threshold = capacity() >> shift. Example: shift=2 -> 1/4 capacity.
 */
#ifndef SPSC_ENABLE_SHADOW_INDICES
#  define SPSC_ENABLE_SHADOW_INDICES 1
#endif /* SPSC_ENABLE_SHADOW_INDICES */

#ifndef SPSC_SHADOW_ALLOW_32BIT
#  define SPSC_SHADOW_ALLOW_32BIT 0
#endif /* SPSC_SHADOW_ALLOW_32BIT */

#ifndef SPSC_SHADOW_REFRESH_HEURISTIC
#  define SPSC_SHADOW_REFRESH_HEURISTIC 0
#endif /* SPSC_SHADOW_REFRESH_HEURISTIC */

#ifndef SPSC_SHADOW_REFRESH_FRAC_SHIFT
#  define SPSC_SHADOW_REFRESH_FRAC_SHIFT 2
#endif /* SPSC_SHADOW_REFRESH_FRAC_SHIFT */


// assert ------------------------
#ifndef SPSC_ASSERT
#  define SPSC_ASSERT(x)
#endif /* SPSC_ASSERT */


/* Legacy default-policy configuration override.
 *
 * Do not define this macro here. In v3 its presence is meaningful:
 *   undefined -> policy::FA<> (the modern concurrent default)
 *   0         -> policy::P    (legacy explicit plain default)
 *   1         -> policy::A<>  (legacy explicit strict-atomic default)
 *
 * New code should select an explicit semantic alias or policy instead of
 * setting this build-wide compatibility override.
 */

/* --------------------------------------------------------------------
 * Optional: require lock-free atomics or allow fallback toolchains.
 *   - Set SPSC_REQUIRE_LOCK_FREE=1 to hard-fail when std::atomic<U> is not always lock-free.
 *   - Default 1 keeps accidental lock-based atomic fallback out of the stable API line.
 * -------------------------------------------------------------------- */
#ifndef SPSC_REQUIRE_LOCK_FREE
#  define SPSC_REQUIRE_LOCK_FREE 1
#endif /* SPSC_REQUIRE_LOCK_FREE */

// ============================================================================
// Exceptions configuration
// ============================================================================
//
// Single switch:
//   - SPSC_ENABLE_EXCEPTIONS == 0 : library assumes "no exceptions" mode.
//   - SPSC_ENABLE_EXCEPTIONS == 1 : library may use throwing paths.
//
// Default: 0 (no exceptions).
//

#ifndef SPSC_ENABLE_EXCEPTIONS
#  define SPSC_ENABLE_EXCEPTIONS 0
#endif /* SPSC_ENABLE_EXCEPTIONS */

#if (SPSC_ENABLE_EXCEPTIONS != 0) && (SPSC_ENABLE_EXCEPTIONS != 1)
#  error "SPSC_ENABLE_EXCEPTIONS must be 0 or 1"
#endif

/*
 * Optional: prefer aligned-new when available.
 */
#ifndef SPSC_ALLOC_PREFER_ALIGNED_NEW
#  define SPSC_ALLOC_PREFER_ALIGNED_NEW 0
#endif /* SPSC_ALLOC_PREFER_ALIGNED_NEW */



#endif /* SPSC_CONFIG_HPP_ */
