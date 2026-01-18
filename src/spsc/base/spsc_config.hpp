/*
 * spsc_config.hpp
 *
 *  Created on: 30 Nov. 2025
 *      Author: Shpegun60
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


/* Optional: globally choose safe-by-default with atomics.
 * 0 = fastest single-core by default (Plain), 1 = Atomic by default.
 */
#ifndef SPSC_DEFAULT_POLICY_ATOMIC
#  define SPSC_DEFAULT_POLICY_ATOMIC 0
#endif /* SPSC_DEFAULT_POLICY_ATOMIC */

/* --------------------------------------------------------------------
 * Optional: require lock-free atomics or allow fallback toolchains.
 *   - Set SPSC_REQUIRE_LOCK_FREE=1 to hard-fail when std::atomic<U> is not always lock-free.
 *   - Default 0 keeps portability (some MCUs need libatomic or are not LF).
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

/*
 * Optional: prefer aligned-new when available.
 */
#ifndef SPSC_ALLOC_PREFER_ALIGNED_NEW
#  define SPSC_ALLOC_PREFER_ALIGNED_NEW 0
#endif /* SPSC_ALLOC_PREFER_ALIGNED_NEW */



#endif /* SPSC_CONFIG_HPP_ */
