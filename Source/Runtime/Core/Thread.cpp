#include "Runtime/Core/Thread.h"

#include <stdexcept>

namespace MmdLab
{
Thread::Thread(Runnable& runnable, const std::wstring_view name)
    : runnable_(&runnable)
    , name_(name)
{
    initEvent_ = CreateEventW(nullptr, false, false, nullptr);
    if (initEvent_ == nullptr)
    {
        throw std::runtime_error("Failed to create the MMDLab thread initialization event.");
    }

    DWORD threadId = 0;
    handle_ = CreateThread(
        nullptr,
        0,
        ThreadEntry,
        this,
        CREATE_SUSPENDED,
        &threadId);

    if (handle_ == nullptr)
    {
        CloseHandle(initEvent_);
        initEvent_ = nullptr;
        throw std::runtime_error("Failed to create an MMDLab thread.");
    }

    threadId_ = threadId;
    if (ResumeThread(handle_) == static_cast<DWORD>(-1))
    {
        // The thread never ran, so it holds no locks or runnable state. Close the handles
        // and fail instead of waiting forever on an event that will never be signaled.
        CloseHandle(handle_);
        handle_ = nullptr;
        CloseHandle(initEvent_);
        initEvent_ = nullptr;
        throw std::runtime_error("Failed to resume the MMDLab thread.");
    }

    // Block until Runnable::Init() has completed so a constructed Thread is already
    // initialized, and so initEvent_ is no longer needed before it is closed below.
    WaitForSingleObject(initEvent_, INFINITE);

    CloseHandle(initEvent_);
    initEvent_ = nullptr;
}

Thread::~Thread()
{
    if (handle_ != nullptr)
    {
        RequestStop();
    }
}

void Thread::RequestStop()
{
    if (handle_ == nullptr)
    {
        return;
    }

    runnable_->Stop();
    Join();
}

void Thread::Join()
{
    if (handle_ == nullptr)
    {
        return;
    }

    WaitForSingleObject(handle_, INFINITE);

    CloseHandle(handle_);
    handle_ = nullptr;
}

DWORD WINAPI Thread::ThreadEntry(void* parameter)
{
    auto* thread = static_cast<Thread*>(parameter);
    return static_cast<DWORD>(thread->RunInternal());
}

uint32_t Thread::RunInternal()
{
    if (!runnable_->Init())
    {
        // Initialization failed; there is no Run() or Exit() to perform.
        SetEvent(initEvent_);
        exitCode_.store(InitFailureExitCode);
        return InitFailureExitCode;
    }

    SetEvent(initEvent_);

    const uint32_t exitCode = runnable_->Run();
    runnable_->Exit();
    exitCode_.store(exitCode);
    return exitCode;
}
} // namespace MmdLab
