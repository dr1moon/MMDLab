#pragma once

#include <cstdint>
#include <string_view>

namespace MmdLab
{
// Severity levels for log output.
enum class LogLevel : std::uint8_t
{
    Verbose,
    Info,
    Warning,
    Error,
};

// Writes one diagnostic line to the log sink (standard error) as
// "[elapsedMs][Level][Category][Thread] message", where Thread is the calling thread's
// registered role name (see RegisterThreadName) or "tid:<id>" when it has none. Thread-safe: the
// asset I/O workers, the render pipeline, and the game thread may log concurrently, so this is
// the single output path for diagnostics and replaces ad-hoc std::cerr writes. `category` names
// the logging subsystem (for example "Asset" or "Dx12"); `message` is one line without a
// trailing newline.
void Log(LogLevel level, std::string_view category, std::string_view message);

// Registers a friendly role name for the calling thread (e.g. "GameThread", "RhiThread",
// "IoThreadsGroup-3") so log lines identify their source by role instead of a raw OS thread id.
// Threads owned by a Thread object register automatically; the main thread registers itself
// explicitly at startup. The name is keyed by the calling thread's OS id and read by Log() on
// every line.
void RegisterThreadName(std::string_view name);

// Removes the calling thread's registered name; called when a worker thread finishes so a later
// thread that reuses the OS id does not inherit a stale label.
void UnregisterThreadName();

// Convenience wrappers for the common severities.
void LogInfo(std::string_view category, std::string_view message);
void LogWarning(std::string_view category, std::string_view message);
void LogError(std::string_view category, std::string_view message);
} // namespace MmdLab
