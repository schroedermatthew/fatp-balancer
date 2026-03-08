/**
 * @file test_FaultInjector_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for FaultInjector.h.
 */

#include "sim/FaultInjector.h"
#include "sim/FaultInjector.h"  // Validate idempotence

int main()
{
    return 0;
}
