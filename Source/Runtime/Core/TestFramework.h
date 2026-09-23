#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>
#include <vector>

namespace MmdLab
{
using TestFunction = void (*)(class TestContext& context);

struct TestCase
{
    std::string_view name;
    TestFunction function;
};

struct TestFailure
{
    std::string_view expression;
    std::source_location location;
};

class TestContext final
{
public:
    void Check(bool condition, std::string_view expression, std::source_location location);

    template <typename Expected, typename Actual>
    void CheckEqual(
        const Expected& expected,
        const Actual& actual,
        std::string_view expectedExpression,
        std::string_view actualExpression,
        std::source_location location)
    {
        if (!(expected == actual))
        {
            Check(false, expectedExpression == actualExpression ? expectedExpression : "values are equal", location);
        }
    }

    [[nodiscard]] uint32_t FailureCount() const;
    [[nodiscard]] const std::vector<TestFailure>& Failures() const;

private:
    std::vector<TestFailure> failures_;
};

class TestRegistration final
{
public:
    TestRegistration(std::string_view name, TestFunction function);
};

struct TestRunSummary
{
    uint32_t testsRun = 0;
    uint32_t testsFailed = 0;
};

[[nodiscard]] TestRunSummary RunTests(std::string_view nameFilter);
}

#define MMDLAB_DETAIL_CONCATENATE_IMPL(left, right) left##right
#define MMDLAB_DETAIL_CONCATENATE(left, right) MMDLAB_DETAIL_CONCATENATE_IMPL(left, right)

#define MMDLAB_TEST(suite, name) MMDLAB_DETAIL_TEST(suite, name, __COUNTER__)
#define MMDLAB_DETAIL_TEST(suite, name, id) \
    static void MMDLAB_DETAIL_CONCATENATE(MmdLabTest_, id)(::MmdLab::TestContext& context); \
    static const ::MmdLab::TestRegistration MMDLAB_DETAIL_CONCATENATE(MmdLabTestRegistration_, id)( \
        #suite "." #name, \
        &MMDLAB_DETAIL_CONCATENATE(MmdLabTest_, id)); \
    static void MMDLAB_DETAIL_CONCATENATE(MmdLabTest_, id)(::MmdLab::TestContext& context)

#define MMDLAB_CHECK(expression) \
    do \
    { \
        context.Check((expression), #expression, std::source_location::current()); \
    } while (false)

#define MMDLAB_CHECK_EQUAL(expected, actual) \
    do \
    { \
        const auto& mmdLabExpectedValue = (expected); \
        const auto& mmdLabActualValue = (actual); \
        context.CheckEqual( \
            mmdLabExpectedValue, \
            mmdLabActualValue, \
            #expected, \
            #actual, \
            std::source_location::current()); \
    } while (false)
