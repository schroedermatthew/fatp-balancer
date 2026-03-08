// test_FeatureSupervisor_HeaderSelfContained.cpp
// Verifies that FeatureSupervisor.h is self-contained: it compiles cleanly
// when included as the first and only project header.

#ifdef ENABLE_TEST_APPLICATION

#include "balancer/FeatureSupervisor.h"

// Structural assertions: key types and methods visible from the include.

static_assert(sizeof(balancer::FeatureSupervisor) > 0,
    "FeatureSupervisor must be a complete type");

int main()
{
    return 0;
}

#endif // ENABLE_TEST_APPLICATION
