#pragma once

#include "Runtime/Core/Runnable.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace MmdLab
{
// Owns one operating-system thread that executes a Runnable.
//
// The Thread starts the thread on construction and joins it on destruction. The caller
// owns the Runnable's storage and must keep it alive until the Thread is destroyed. The
// Thread is not copyable; its lifetime represents the thread's joinable lifetime.
//
// The constructor blocks until Runnable::Init() completes, so a fully constructed Thread
// has already initialized its runnable. If Init() returns false, Run() and Exit() are
// skipped and the thread exits with InitFailureExitCode.
class Thread final
{
public:
    Thread(Runnable& runnable, std::wstring_view name);
    ~Thread();

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Requests graceful termination: calls Runnable::Stop(), then blocks until the thread
    // has finished Run() and Exit(). A no-op if the thread has already been joined.
    void RequestStop();

    // Blocks until the thread has finished Run() and Exit(). Does not signal Stop().
    void Join();

    // Identifier assigned by the operating system when the thread was created.
    [[nodiscard]] uint32_t ThreadId() const { return threadId_; }

    // Name given at construction, retained for diagnostics.
    [[nodiscard]] const std::wstring& Name() const { return name_; }

    // True while the thread has not yet been joined.
    [[nodiscard]] bool IsJoinable() const { return handle_ != nullptr; }

    // Exit code produced by Runnable::Run(); meaningful after Join() or RequestStop().
    [[nodiscard]] uint32_t ExitCode() const { return exitCode_.load(); }

    // Exit code reported when Runnable::Init() returns false.
    static constexpr uint32_t InitFailureExitCode = 0xFFFFFFFFu;

private:
    static DWORD WINAPI ThreadEntry(void* parameter);
    uint32_t RunInternal();

    Runnable* runnable_;
    std::wstring name_;
    HANDLE handle_ = nullptr;
    HANDLE initEvent_ = nullptr;
    uint32_t threadId_ = 0;
    std::atomic<uint32_t> exitCode_{ 0 };
};
} // namespace MmdLab
