#include "Runtime/Core/Log.h"

#include <windows.h>

#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
// Serializes writes from every logging thread and guards the thread-name registry below. This is
// the one piece of shared state the logger needs; it lives at file scope rather than on a global
// object.
std::mutex g_logMutex;

// OS thread id -> friendly role name (e.g. "GameThread", "RhiThread", "IoThreadsGroup-3").
// Filled by RegisterThreadName/UnregisterThreadName and read on every log line to identify the
// source. Keyed by OS id because that is what GetCurrentThreadId() can cheaply produce at log
// time; a map (not an array) because OS thread ids are sparse.
std::unordered_map<DWORD, std::string> g_threadNames;

const char* LevelName(const MmdLab::LogLevel level)
{
    switch (level)
    {
        case MmdLab::LogLevel::Verbose: return "Verbose";
        case MmdLab::LogLevel::Info:    return "Info";
        case MmdLab::LogLevel::Warning: return "Warning";
        case MmdLab::LogLevel::Error:   return "Error";
    }
    return "Unknown";
}
} // namespace

namespace MmdLab
{
void Log(const LogLevel level, const std::string_view category, const std::string_view message)
{
    static const auto processStart = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - processStart).count();

    std::lock_guard lock(g_logMutex);
    const DWORD threadId = GetCurrentThreadId();
    const auto thread = g_threadNames.find(threadId);
    const std::string threadLabel = thread != g_threadNames.end() ? thread->second : std::format("tid:{}", threadId);
    std::cerr << '[' << elapsedMs << "ms][" << LevelName(level) << "][" << category << "]["
              << threadLabel << "] " << message << '\n';
}

void RegisterThreadName(const std::string_view name)
{
    std::lock_guard lock(g_logMutex);
    g_threadNames[GetCurrentThreadId()] = std::string(name);
}

void UnregisterThreadName()
{
    std::lock_guard lock(g_logMutex);
    g_threadNames.erase(GetCurrentThreadId());
}

void LogInfo(const std::string_view category, const std::string_view message)
{
    Log(LogLevel::Info, category, message);
}

void LogWarning(const std::string_view category, const std::string_view message)
{
    Log(LogLevel::Warning, category, message);
}

void LogError(const std::string_view category, const std::string_view message)
{
    Log(LogLevel::Error, category, message);
}
} // namespace MmdLab
