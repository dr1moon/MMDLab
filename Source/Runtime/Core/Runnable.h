#pragma once

#include <cstdint>

namespace MmdLab
{
// Interface for an object that runs on an operating-system thread owned by a Thread.
//
// The owning Thread invokes Init(), Run(), then Exit() on the new thread. A Runnable
// performs its thread-local setup in Init(), does the thread's work in Run(), and
// releases thread-local resources in Exit(). Stop() is called from another thread to
// request graceful early termination.
//
// Init(), Run(), and Exit() must not throw; an uncaught exception escaping the thread's
// entry point terminates the process.
class Runnable
{
public:
    virtual ~Runnable() = default;

    // Called on the new thread before Run(). Return false to skip Run() and Exit().
    virtual bool Init() { return true; }

    // Called on the new thread after a successful Init(). Returns the thread's exit code.
    virtual uint32_t Run() = 0;

    // Called from another thread to request graceful early termination. The default is a
    // no-op; a runnable that can stop early overrides this to set a signal Run() observes.
    virtual void Stop() {}

    // Called on the new thread after Run() returns, to release thread-local resources.
    virtual void Exit() {}
};
} // namespace MmdLab
