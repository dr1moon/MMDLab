#include "Runtime/Core/TestFramework.h"

#include <cstdio>
#include <string_view>

int main(const int argumentCount, char* arguments[])
{
    std::string_view nameFilter;

    if (argumentCount == 2)
    {
        nameFilter = arguments[1];
    }
    else if (argumentCount > 2)
    {
        std::printf("Usage: MmdTests.exe [name-filter]\n");
        return 1;
    }

    const MmdLab::TestRunSummary summary = MmdLab::RunTests(nameFilter);
    return summary.testsFailed == 0 ? 0 : 1;
}
