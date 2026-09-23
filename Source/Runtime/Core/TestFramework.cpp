#include "Runtime/Core/TestFramework.h"

#include <chrono>
#include <cstdio>
#include <exception>

namespace
{
std::vector<MmdLab::TestCase>& GetTestCases()
{
    static std::vector<MmdLab::TestCase> testCases;
    return testCases;
}

bool MatchesFilter(const std::string_view testName, const std::string_view nameFilter)
{
    return nameFilter.empty() || testName.find(nameFilter) != std::string_view::npos;
}
}

namespace MmdLab
{
void TestContext::Check(const bool condition, const std::string_view expression, const std::source_location location)
{
    if (!condition)
    {
        failures_.push_back({ expression, location });
    }
}

uint32_t TestContext::FailureCount() const
{
    return static_cast<uint32_t>(failures_.size());
}

const std::vector<TestFailure>& TestContext::Failures() const
{
    return failures_;
}

TestRegistration::TestRegistration(const std::string_view name, const TestFunction function)
{
    GetTestCases().push_back({ name, function });
}

TestRunSummary RunTests(const std::string_view nameFilter)
{
    TestRunSummary summary;

    for (const TestCase& testCase : GetTestCases())
    {
        if (!MatchesFilter(testCase.name, nameFilter))
        {
            continue;
        }

        ++summary.testsRun;
        TestContext context;
        const auto startTime = std::chrono::steady_clock::now();

        try
        {
            testCase.function(context);
        }
        catch (const std::exception& exception)
        {
            context.Check(false, exception.what(), std::source_location::current());
        }
        catch (...)
        {
            context.Check(false, "unknown exception", std::source_location::current());
        }

        const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);

        if (context.FailureCount() == 0)
        {
            std::printf("PASS %.*s (%lld ms)\n", static_cast<int>(testCase.name.size()), testCase.name.data(), elapsedMilliseconds.count());
            continue;
        }

        ++summary.testsFailed;
        std::printf("FAIL %.*s (%lld ms)\n", static_cast<int>(testCase.name.size()), testCase.name.data(), elapsedMilliseconds.count());
        for (const TestFailure& failure : context.Failures())
        {
            std::printf(
                "  %s(%u): %.*s\n",
                failure.location.file_name(),
                failure.location.line(),
                static_cast<int>(failure.expression.size()),
                failure.expression.data());
        }
    }

    std::printf("Tests run: %u, failed: %u\n", summary.testsRun, summary.testsFailed);
    return summary;
}
}
