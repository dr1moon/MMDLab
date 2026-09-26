#include "Runtime/Core/TestFramework.h"
#include "Runtime/Core/Thread.h"

#include <atomic>
#include <thread>

namespace
{
class CountingRunnable final : public MmdLab::Runnable
{
public:
    bool Init() override
    {
        initCalled = true;
        return true;
    }

    uint32_t Run() override
    {
        runCalled = true;
        return kExitCode;
    }

    void Exit() override
    {
        exitCalled = true;
    }

    static constexpr uint32_t kExitCode = 7;

    std::atomic<bool> initCalled{ false };
    std::atomic<bool> runCalled{ false };
    std::atomic<bool> exitCalled{ false };
};

class FailingInitRunnable final : public MmdLab::Runnable
{
public:
    bool Init() override
    {
        return false;
    }

    uint32_t Run() override
    {
        runCalled = true;
        return 0;
    }

    void Exit() override
    {
        exitCalled = true;
    }

    std::atomic<bool> runCalled{ false };
    std::atomic<bool> exitCalled{ false };
};

class StoppableRunnable final : public MmdLab::Runnable
{
public:
    uint32_t Run() override
    {
        while (!stopRequested.load())
        {
            std::this_thread::yield();
        }
        return 0;
    }

    void Stop() override
    {
        stopCalled = true;
        stopRequested = true;
    }

    std::atomic<bool> stopCalled{ false };

private:
    std::atomic<bool> stopRequested{ false };
};
} // namespace

MMDLAB_TEST(Core.Thread, ConstructorWaitsForInitToComplete)
{
    CountingRunnable runnable;
    MmdLab::Thread thread(runnable, L"InitSync");

    // Init() has completed before the constructor returned.
    MMDLAB_CHECK(runnable.initCalled.load());

    thread.Join();
}

MMDLAB_TEST(Core.Thread, RunsInitRunExitInOrder)
{
    CountingRunnable runnable;
    {
        MmdLab::Thread thread(runnable, L"Lifecycle");
        thread.Join();
    }

    MMDLAB_CHECK(runnable.initCalled.load());
    MMDLAB_CHECK(runnable.runCalled.load());
    MMDLAB_CHECK(runnable.exitCalled.load());
}

MMDLAB_TEST(Core.Thread, ReportsRunnableExitCode)
{
    CountingRunnable runnable;
    MmdLab::Thread thread(runnable, L"ExitCode");
    thread.Join();

    MMDLAB_CHECK_EQUAL(CountingRunnable::kExitCode, thread.ExitCode());
}

MMDLAB_TEST(Core.Thread, InitFailureSkipsRunAndExit)
{
    FailingInitRunnable runnable;
    MmdLab::Thread thread(runnable, L"InitFail");
    thread.Join();

    MMDLAB_CHECK(!runnable.runCalled.load());
    MMDLAB_CHECK(!runnable.exitCalled.load());
    MMDLAB_CHECK_EQUAL(MmdLab::Thread::InitFailureExitCode, thread.ExitCode());
}

MMDLAB_TEST(Core.Thread, RequestStopTerminatesAndJoins)
{
    StoppableRunnable runnable;
    MmdLab::Thread thread(runnable, L"Stop");

    thread.RequestStop();

    MMDLAB_CHECK(runnable.stopCalled.load());
    MMDLAB_CHECK(!thread.IsJoinable());
}

MMDLAB_TEST(Core.Thread, DestructorJoinsAStillRunningThread)
{
    StoppableRunnable runnable;
    {
        MmdLab::Thread thread(runnable, L"DestructorStop");
    }

    MMDLAB_CHECK(runnable.stopCalled.load());
}
