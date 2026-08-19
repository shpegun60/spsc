#include "fifo.hpp"

// The compile-fail harness supplies one zero-valued cacheline configuration
// macro at a time. Every such configuration must be rejected before any
// container instantiation can depend on it.
int main()
{
    return 0;
}
