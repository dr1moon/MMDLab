#include "Runtime/Core/Channel.h"
#include "Runtime/Core/TestFramework.h"

#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace
{
constexpr std::size_t kCapacity = 4;
using TestChannel = MmdLab::Channel<int, kCapacity>;
} // namespace

MMDLAB_TEST(Core.Channel, IsEmptyInitially)
{
    TestChannel channel;
    MMDLAB_CHECK(channel.IsEmpty());
    MMDLAB_CHECK(!channel.IsFull());
    MMDLAB_CHECK_EQUAL(0u, channel.Size());
}

MMDLAB_TEST(Core.Channel, PushPopPreservesFifoOrder)
{
    TestChannel channel;
    channel.Push(10);
    channel.Push(20);
    channel.Push(30);

    MMDLAB_CHECK_EQUAL(10, *channel.Pop());
    MMDLAB_CHECK_EQUAL(20, *channel.Pop());
    MMDLAB_CHECK_EQUAL(30, *channel.Pop());
    MMDLAB_CHECK(channel.IsEmpty());
}

MMDLAB_TEST(Core.Channel, TryPushFailsWhenFull)
{
    TestChannel channel;
    for (std::size_t i = 0; i < kCapacity; ++i)
    {
        MMDLAB_CHECK(channel.TryPush(static_cast<int>(i)));
    }

    MMDLAB_CHECK(channel.IsFull());
    MMDLAB_CHECK(!channel.TryPush(99));
}

MMDLAB_TEST(Core.Channel, TryPopReturnsNulloptWhenEmpty)
{
    TestChannel channel;
    MMDLAB_CHECK(!channel.TryPop().has_value());
}

MMDLAB_TEST(Core.Channel, PopBlocksUntilProducerPushes)
{
    TestChannel channel;
    std::atomic<bool> popped{ false };
    std::atomic<int> value{ -1 };

    std::thread consumer([&channel, &popped, &value] {
        value = *channel.Pop();
        popped = true;
    });

    std::this_thread::yield();
    MMDLAB_CHECK(!popped.load());

    channel.Push(42);
    consumer.join();

    MMDLAB_CHECK(popped.load());
    MMDLAB_CHECK_EQUAL(42, value.load());
}

MMDLAB_TEST(Core.Channel, PushBlocksUntilConsumerPops)
{
    TestChannel channel;
    for (std::size_t i = 0; i < kCapacity; ++i)
    {
        channel.Push(static_cast<int>(i));
    }

    std::atomic<bool> pushed{ false };

    std::thread producer([&channel, &pushed] {
        channel.Push(99);
        pushed = true;
    });

    std::this_thread::yield();
    MMDLAB_CHECK(!pushed.load());

    (void)channel.Pop();
    producer.join();

    MMDLAB_CHECK(pushed.load());
}

MMDLAB_TEST(Core.Channel, ProducerConsumerRoundTrip)
{
    constexpr int kItemCount = 200;
    TestChannel channel;
    std::array<int, kItemCount> received{};

    std::thread consumer([&channel, &received] {
        for (int i = 0; i < kItemCount; ++i)
        {
            received[static_cast<std::size_t>(i)] = *channel.Pop();
        }
    });

    for (int i = 0; i < kItemCount; ++i)
    {
        channel.Push(i);
    }

    consumer.join();

    for (int i = 0; i < kItemCount; ++i)
    {
        MMDLAB_CHECK_EQUAL(i, received[static_cast<std::size_t>(i)]);
    }
}

MMDLAB_TEST(Core.Channel, PopReturnsNulloptWhenClosedEmpty)
{
    TestChannel channel;
    channel.Close();
    MMDLAB_CHECK(!channel.Pop().has_value());
}

MMDLAB_TEST(Core.Channel, CloseDrainsRemainingItemsThenNullopt)
{
    TestChannel channel;
    channel.Push(1);
    channel.Push(2);
    channel.Close();

    MMDLAB_CHECK_EQUAL(1, *channel.Pop());
    MMDLAB_CHECK_EQUAL(2, *channel.Pop());
    MMDLAB_CHECK(!channel.Pop().has_value());
}

MMDLAB_TEST(Core.Channel, CloseWakesBlockedPop)
{
    TestChannel channel;
    std::atomic<bool> returned{ false };
    std::atomic<bool> receivedValue{ false };

    std::thread consumer([&channel, &returned, &receivedValue] {
        const auto value = channel.Pop();
        receivedValue = value.has_value();
        returned = true;
    });

    std::this_thread::yield();
    MMDLAB_CHECK(!returned.load());

    channel.Close();
    consumer.join();

    MMDLAB_CHECK(returned.load());
    MMDLAB_CHECK(!receivedValue.load());
}

MMDLAB_TEST(Core.Channel, PushAfterCloseThrows)
{
    TestChannel channel;
    channel.Close();

    bool threw = false;
    try
    {
        channel.Push(1);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    MMDLAB_CHECK(threw);
}

MMDLAB_TEST(Core.Channel, TryPushAfterCloseReturnsFalse)
{
    TestChannel channel;
    channel.Close();
    MMDLAB_CHECK(!channel.TryPush(1));
}
