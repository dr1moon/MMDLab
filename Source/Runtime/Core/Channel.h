#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace MmdLab
{
// A bounded channel that moves items from a single producer thread to a single consumer
// thread. Each edge of the runtime data graph carries a FrameIndex through one of these.
//
// The access contract is single-producer, single-consumer (SPSC): exactly one thread calls
// the producer methods and exactly one thread calls the consumer methods, which matches
// the edge's single writer / single reader ownership rule. The implementation is a fixed
// ring guarded by a mutex plus two condition variables, so it stays correct even if misused
// by more than two threads; "SPSC" names the usage contract, not an implementation-level
// concurrency restriction. A lock-free variant would keep the same contract and change only
// the implementation.
//
// A channel is open or closed. Close() moves it to closed and wakes any blocked consumer.
// Pop() drains the remaining items first, then returns std::nullopt once the channel is both
// closed and empty, so a consumer loop can exit gracefully after draining:
//
//     while (auto item = channel.Pop())
//     {
//         Process(*item);
//     }
//
// T must be default-constructible and movable.
template <typename T, std::size_t Capacity>
class Channel final
{
    static_assert(Capacity >= 2, "A channel ring needs room for at least two elements.");

public:
    Channel() = default;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Closes the channel. A consumer blocked in Pop() wakes, drains any remaining items,
    // then receives std::nullopt. Producers blocked in Push() also wake and throw.
    void Close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // Producer: blocks until there is room, then stores the value. Throws if the channel is
    // closed now or becomes closed while waiting.
    void Push(T value)
    {
        std::unique_lock lock(mutex_);
        if (closed_)
        {
            throw std::runtime_error("Channel::Push: the channel is closed.");
        }
        notFull_.wait(lock, [this] { return count_ < Capacity || closed_; });
        if (closed_)
        {
            throw std::runtime_error("Channel::Push: the channel was closed while waiting.");
        }
        EmplaceUnchecked(std::move(value));
        lock.unlock();
        notEmpty_.notify_one();
    }

    // Producer: stores the value if there is room; returns false when full or closed.
    bool TryPush(T value)
    {
        std::unique_lock lock(mutex_);
        if (closed_ || count_ == Capacity)
        {
            return false;
        }
        EmplaceUnchecked(std::move(value));
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    // Consumer: blocks until an item is available or the channel is closed. Drains the
    // remaining items first, then returns std::nullopt once closed and empty.
    std::optional<T> Pop()
    {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [this] { return count_ > 0 || closed_; });
        if (count_ == 0)
        {
            return std::nullopt;
        }
        T value = TakeUnchecked();
        lock.unlock();
        notFull_.notify_one();
        return value;
    }

    // Consumer: returns the item, or std::nullopt when the channel is empty.
    std::optional<T> TryPop()
    {
        std::unique_lock lock(mutex_);
        if (count_ == 0)
        {
            return std::nullopt;
        }
        T value = TakeUnchecked();
        lock.unlock();
        notFull_.notify_one();
        return value;
    }

    [[nodiscard]] bool IsEmpty() const
    {
        std::lock_guard lock(mutex_);
        return count_ == 0;
    }

    [[nodiscard]] bool IsFull() const
    {
        std::lock_guard lock(mutex_);
        return count_ == Capacity;
    }

    [[nodiscard]] bool IsClosed() const
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t Size() const
    {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    static std::size_t Advance(const std::size_t index)
    {
        return index + 1 == Capacity ? 0 : index + 1;
    }

    void EmplaceUnchecked(T value)
    {
        buffer_[tail_] = std::move(value);
        tail_ = Advance(tail_);
        ++count_;
    }

    T TakeUnchecked()
    {
        T value = std::move(buffer_[head_]);
        head_ = Advance(head_);
        --count_;
        return value;
    }

    std::array<T, Capacity> buffer_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    bool closed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
};
} // namespace MmdLab
