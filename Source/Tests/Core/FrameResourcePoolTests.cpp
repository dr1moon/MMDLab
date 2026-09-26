#include "Runtime/Core/FrameResourcePool.h"
#include "Runtime/Core/TestFramework.h"

#include <atomic>
#include <stdexcept>
#include <thread>

MMDLAB_TEST(Core.FrameResourcePool, StartsWithAllFramesFree)
{
    MmdLab::FrameResourcePool pool;
    MMDLAB_CHECK_EQUAL(MmdLab::FrameResourcePool::kFrameCount, pool.FreeCount());
}

MMDLAB_TEST(Core.FrameResourcePool, AcquireHandsOutDistinctIndices)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex a = pool.Acquire();
    const MmdLab::FrameIndex b = pool.Acquire();
    const MmdLab::FrameIndex c = pool.Acquire();

    MMDLAB_CHECK(a != b);
    MMDLAB_CHECK(a != c);
    MMDLAB_CHECK(b != c);
    MMDLAB_CHECK_EQUAL(0u, pool.FreeCount());
}

MMDLAB_TEST(Core.FrameResourcePool, ReleaseReturnsFrameToFreePool)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex a = pool.Acquire();
    MMDLAB_CHECK_EQUAL(MmdLab::FrameResourcePool::kFrameCount - 1, pool.FreeCount());

    pool.Release(a);
    MMDLAB_CHECK_EQUAL(MmdLab::FrameResourcePool::kFrameCount, pool.FreeCount());

    const MmdLab::FrameIndex again = pool.Acquire();
    MMDLAB_CHECK_EQUAL(a, again);
}

MMDLAB_TEST(Core.FrameResourcePool, AcquireBlocksWhenExhausted)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex a = pool.Acquire();
    (void)pool.Acquire();
    (void)pool.Acquire();

    std::atomic<bool> acquired{ false };

    std::thread waiter([&pool, &acquired] {
        (void)pool.Acquire();
        acquired = true;
    });

    std::this_thread::yield();
    MMDLAB_CHECK(!acquired.load());

    pool.Release(a);
    waiter.join();

    MMDLAB_CHECK(acquired.load());
}

MMDLAB_TEST(Core.FrameResourcePool, GetReturnsMutableFrame)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex index = pool.Acquire();

    MmdLab::FrameResource& frame = pool.Get(index);
    frame.frameId = static_cast<MmdLab::FrameId>(12345);
    MMDLAB_CHECK_EQUAL(static_cast<MmdLab::FrameId>(12345), pool.Get(index).frameId);
}

MMDLAB_TEST(Core.FrameResourcePool, ReleaseThrowsOnOutOfRangeIndex)
{
    MmdLab::FrameResourcePool pool;
    (void)pool.Acquire();  // Leave the pool not-full so only the range check can trip.

    bool threw = false;
    try
    {
        pool.Release(static_cast<MmdLab::FrameIndex>(3));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    MMDLAB_CHECK(threw);
}

MMDLAB_TEST(Core.FrameResourcePool, ReleaseThrowsOnDoubleRelease)
{
    MmdLab::FrameResourcePool pool;
    const MmdLab::FrameIndex a = pool.Acquire();
    (void)pool.Acquire();  // Keep one frame held so the pool is not all-free.

    pool.Release(a);

    bool threw = false;
    try
    {
        pool.Release(a);  // a is already free.
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    MMDLAB_CHECK(threw);
}
