// Hostile Windows macro smoke.
//
// Includes <windows.h> deliberately WITHOUT NOMINMAX so the function-like
// min/max macros are active while every public SPSC header compiles. The
// library must survive this by using the (std::numeric_limits<T>::max)()
// idiom everywhere; it must never #undef the user's macros.
//
// Reuses the full h7 header smoke body so the entire public API surface is
// instantiated with the hostile macros active.
#include <windows.h>

#if !defined(max) || !defined(min)
#  error "windows minmax smoke requires the hostile min/max macros to be active"
#endif

#include "h7_header_smoke.cpp"
