#include "Runtime/Core/TestFramework.h"

#include <source_location>

MMDLAB_TEST(Core.TestFramework, ChecksSuccessfulConditions)
{
    MMDLAB_CHECK(true);
    MMDLAB_CHECK_EQUAL(42, 42);
}

MMDLAB_TEST(Core.TestFramework, CapturesFailureMetadata)
{
    MmdLab::TestContext localContext;
    localContext.Check(false, "expected failure", std::source_location::current());

    MMDLAB_CHECK_EQUAL(1U, localContext.FailureCount());
    MMDLAB_CHECK_EQUAL("expected failure", localContext.Failures().front().expression);
}
