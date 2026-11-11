// Copyright (C) Microsoft Corporation. All rights reserved.

#include "S16LePcmRing.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

using wsl::virtio_sound::native::PcmFormat;
using wsl::virtio_sound::native::PcmRingWriteResult;
using wsl::virtio_sound::native::S16LePcmRingSource;

namespace {
constexpr std::uint32_t c_concurrentBatchFrames = 8;
constexpr std::uint32_t c_concurrentTotalFrames = 40000;
constexpr std::size_t c_concurrentBatchBytes = static_cast<std::size_t>(c_concurrentBatchFrames) * 4;

void Assert(bool condition, const char* message, int& assertions)
{
    ++assertions;
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void EncodeFrame(std::uint32_t sequence, std::uint8_t* destination)
{
    const std::uint16_t left = static_cast<std::uint16_t>(sequence);
    const std::uint16_t right = static_cast<std::uint16_t>(sequence ^ 0xa55aU);
    destination[0] = static_cast<std::uint8_t>(left);
    destination[1] = static_cast<std::uint8_t>(left >> 8);
    destination[2] = static_cast<std::uint8_t>(right);
    destination[3] = static_cast<std::uint8_t>(right >> 8);
}

void EncodeFrames(std::uint32_t firstSequence, std::size_t count, std::uint8_t* destination)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        EncodeFrame(firstSequence + static_cast<std::uint32_t>(i), destination + i * 4);
    }
}

enum class ConsumerBatchRead
{
    Read,
    Retry,
    Finished,
};

template <typename AfterEmptyRead>
ConsumerBatchRead ReadConsumerBatch(
    S16LePcmRingSource& ring,
    const std::atomic<bool>& producerDone,
    std::uint32_t frames,
    std::uint8_t* destination,
    std::size_t byteCount,
    AfterEmptyRead&& afterEmptyRead)
{
    // Observe completion first. If it is true, the acquire pairs with the
    // producer's release after its final ring publication, so the read below
    // cannot mistake that final batch for an empty ring.
    const bool producerFinishedBeforeRead = producerDone.load(std::memory_order_acquire);
    if (ring.ReadFrames(frames, destination, byteCount))
    {
        return ConsumerBatchRead::Read;
    }

    afterEmptyRead();
    return producerFinishedBeforeRead ? ConsumerBatchRead::Finished : ConsumerBatchRead::Retry;
}

void TestOverflowUnderflowWholeFramesAndWrap(int& assertions)
{
    S16LePcmRingSource ring(PcmFormat{2, 48000}, 4);
    const std::uint64_t generation = ring.Generation();
    std::array<std::uint8_t, 12> first{};
    EncodeFrames(10, 3, first.data());
    Assert(
        ring.TryWriteFrames(generation, first.data(), 3, first.size()) == PcmRingWriteResult::Written,
        "Three complete stereo frames are accepted",
        assertions);

    std::array<std::uint8_t, 16> output{};
    output.fill(0xa5);
    Assert(
        !ring.ReadFrames(4, output.data(), output.size()), "Underflow rejects the entire read instead of returning partial frames", assertions);
    Assert(
        output[0] == 0xa5 && output[15] == 0xa5 && ring.Snapshot().m_queuedFrames == 3,
        "Underflow preserves destination and queued frames",
        assertions);

    std::array<std::uint8_t, 8> overflow{};
    EncodeFrames(20, 2, overflow.data());
    Assert(
        ring.TryWriteFrames(generation, overflow.data(), 2, overflow.size()) == PcmRingWriteResult::Overflow,
        "Insufficient free capacity rejects a complete producer batch",
        assertions);
    Assert(ring.Snapshot().m_queuedFrames == 3, "Overflow leaves already queued frames untouched", assertions);

    std::array<std::uint8_t, 12> readFirst{};
    Assert(ring.ReadFrames(3, readFirst.data(), readFirst.size()), "Queued whole frames remain readable after overflow", assertions);
    std::array<std::uint8_t, 12> expectedFirst{};
    EncodeFrames(10, 3, expectedFirst.data());
    Assert(readFirst == expectedFirst, "Stereo samples retain exact interleaved S16_LE bytes", assertions);

    std::array<std::uint8_t, 12> wrappedStart{};
    EncodeFrames(30, 3, wrappedStart.data());
    Assert(
        ring.TryWriteFrames(generation, wrappedStart.data(), 3, wrappedStart.size()) == PcmRingWriteResult::Written,
        "Producer writes a batch that wraps at the ring end",
        assertions);
    std::array<std::uint8_t, 8> partial{};
    Assert(ring.ReadFrames(2, partial.data(), partial.size()), "Consumer advances by complete frames across a wrap", assertions);
    std::array<std::uint8_t, 8> expectedPartial{};
    EncodeFrames(30, 2, expectedPartial.data());
    Assert(partial == expectedPartial, "Wrapped prefix preserves channel ordering", assertions);

    std::array<std::uint8_t, 12> wrappedEnd{};
    EncodeFrames(40, 3, wrappedEnd.data());
    Assert(
        ring.TryWriteFrames(generation, wrappedEnd.data(), 3, wrappedEnd.size()) == PcmRingWriteResult::Written,
        "Producer can refill the vacated wrapped slots",
        assertions);
    std::array<std::uint8_t, 16> complete{};
    Assert(ring.ReadFrames(4, complete.data(), complete.size()), "Consumer reads a full ring spanning both storage segments", assertions);
    std::array<std::uint8_t, 16> expectedComplete{};
    EncodeFrame(32, expectedComplete.data());
    EncodeFrames(40, 3, expectedComplete.data() + 4);
    Assert(complete == expectedComplete, "Ring wrap preserves frame sequence and channel bytes", assertions);

    std::array<std::uint8_t, 4> invalid{};
    Assert(!ring.ReadFrames(1, invalid.data(), 3), "Read rejects a byte count that splits a S16 stereo frame", assertions);
    Assert(
        ring.TryWriteFrames(generation, invalid.data(), 1, 3) == PcmRingWriteResult::InvalidBuffer,
        "Write rejects a byte count that splits a S16 stereo frame",
        assertions);

    const auto snapshot = ring.Snapshot();
    Assert(
        snapshot.m_overflowEvents == 1 && snapshot.m_overflowFramesDropped == 2,
        "Overflow counters report rejected producer frames",
        assertions);
    Assert(
        snapshot.m_underflowEvents == 1 && snapshot.m_underflowFramesMissing == 1,
        "Underflow counters report missing consumer frames",
        assertions);
    Assert(snapshot.m_invalidReads == 1 && snapshot.m_invalidWrites == 1, "Invalid whole-frame requests have separate counters", assertions);
}

void TestResetGeneration(int& assertions)
{
    S16LePcmRingSource ring(PcmFormat{2, 48000}, 8);
    const std::uint64_t oldGeneration = ring.Generation();
    std::array<std::uint8_t, 8> oldSamples{};
    EncodeFrames(100, 2, oldSamples.data());
    Assert(
        ring.TryWriteFrames(oldGeneration, oldSamples.data(), 2, oldSamples.size()) == PcmRingWriteResult::Written,
        "Current-generation frames are accepted before reset",
        assertions);

    std::atomic<bool> producerHasOldBatch{};
    std::atomic<bool> allowOldWrite{};
    std::atomic<bool> oldWriteRejected{};
    std::thread delayedProducer([&] {
        producerHasOldBatch.store(true, std::memory_order_release);
        while (!allowOldWrite.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        oldWriteRejected.store(
            ring.TryWriteFrames(oldGeneration, oldSamples.data(), 2, oldSamples.size()) == PcmRingWriteResult::StaleGeneration,
            std::memory_order_release);
    });
    while (!producerHasOldBatch.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    ring.Reset();
    const std::uint64_t newGeneration = ring.Generation();
    const bool resetAdvancedAndCleared = newGeneration == oldGeneration + 1 && ring.Snapshot().m_queuedFrames == 0;
    allowOldWrite.store(true, std::memory_order_release);
    delayedProducer.join();
    Assert(resetAdvancedAndCleared, "Reset advances generation and discards queued samples", assertions);
    Assert(
        oldWriteRejected.load(std::memory_order_acquire),
        "Producer using a token captured before reset cannot enqueue stale samples",
        assertions);

    std::array<std::uint8_t, 4> output{};
    output.fill(0x7b);
    Assert(
        !ring.ReadFrames(1, output.data(), output.size()) && output[0] == 0x7b,
        "Consumer cannot observe pre-reset frames after reset",
        assertions);

    std::array<std::uint8_t, 8> newSamples{};
    EncodeFrames(200, 2, newSamples.data());
    Assert(
        ring.TryWriteFrames(newGeneration, newSamples.data(), 2, newSamples.size()) == PcmRingWriteResult::Written,
        "Producer using the new generation can write after reset",
        assertions);
    std::array<std::uint8_t, 8> readback{};
    Assert(ring.ReadFrames(2, readback.data(), readback.size()) && readback == newSamples, "Only post-reset samples are returned", assertions);
    const auto snapshot = ring.Snapshot();
    Assert(snapshot.m_resetCount == 1 && snapshot.m_staleGenerationWrites == 1, "Reset and stale-write counters remain observable", assertions);
}

void TestConsumerFinalPublicationRace(int& assertions)
{
    S16LePcmRingSource ring(PcmFormat{2, 48000}, c_concurrentBatchFrames);
    const std::uint64_t generation = ring.Generation();
    std::array<std::uint8_t, c_concurrentBatchBytes> finalBatch{};
    std::array<std::uint8_t, c_concurrentBatchBytes> output{};
    EncodeFrames(700, c_concurrentBatchFrames, finalBatch.data());
    std::atomic<bool> producerDone{};

    const ConsumerBatchRead firstRead = ReadConsumerBatch(
        ring,
        producerDone,
        c_concurrentBatchFrames,
        output.data(),
        output.size(),
        [&] {
            // Deterministically publish the producer's final batch after an
            // empty read but before the consumer decides whether to terminate.
            Assert(
                ring.TryWriteFrames(generation, finalBatch.data(), c_concurrentBatchFrames, finalBatch.size()) ==
                    PcmRingWriteResult::Written,
                "Final producer batch is published after the initial empty read",
                assertions);
            producerDone.store(true, std::memory_order_release);
        });

    Assert(
        firstRead == ConsumerBatchRead::Retry,
        "A read that began before producer completion retries instead of reporting terminal empty",
        assertions);
    Assert(
        producerDone.load(std::memory_order_acquire) && ring.Snapshot().m_queuedFrames == c_concurrentBatchFrames,
        "Producer completion follows publication of the final batch",
        assertions);

    const ConsumerBatchRead finalRead = ReadConsumerBatch(
        ring, producerDone, c_concurrentBatchFrames, output.data(), output.size(), [] {});
    Assert(
        finalRead == ConsumerBatchRead::Read && output == finalBatch,
        "Acquire-before-read exposes the final batch after producer completion",
        assertions);
    Assert(ring.Snapshot().m_queuedFrames == 0, "Final batch is consumed exactly once", assertions);
}

void TestConcurrentProducerConsumer(int& assertions)
{
    S16LePcmRingSource ring(PcmFormat{2, 48000}, 1024);
    const std::uint64_t generation = ring.Generation();
    std::atomic<bool> producerDone{};
    std::atomic<bool> abort{};
    std::atomic<bool> mismatch{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    std::thread producer([&] {
        std::array<std::uint8_t, c_concurrentBatchBytes> input{};
        for (std::uint32_t first = 0; first < c_concurrentTotalFrames && !abort.load(std::memory_order_relaxed);)
        {
            EncodeFrames(first, c_concurrentBatchFrames, input.data());
            const auto result = ring.TryWriteFrames(generation, input.data(), c_concurrentBatchFrames, input.size());
            if (result == PcmRingWriteResult::Written)
            {
                first += c_concurrentBatchFrames;
            }
            else if (result == PcmRingWriteResult::Overflow)
            {
                std::this_thread::yield();
            }
            else
            {
                mismatch.store(true, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                break;
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                abort.store(true, std::memory_order_relaxed);
                break;
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::array<std::uint8_t, c_concurrentBatchBytes> output{};
        std::uint32_t expectedSequence = 0;
        const auto noAfterEmptyRead = [] {};
        while (expectedSequence < c_concurrentTotalFrames && !abort.load(std::memory_order_relaxed))
        {
            const ConsumerBatchRead readResult = ReadConsumerBatch(
                ring,
                producerDone,
                c_concurrentBatchFrames,
                output.data(),
                output.size(),
                noAfterEmptyRead);
            if (readResult == ConsumerBatchRead::Finished)
            {
                mismatch.store(true, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                break;
            }
            if (readResult == ConsumerBatchRead::Retry)
            {
                std::this_thread::yield();
            }
            else
            {
                std::array<std::uint8_t, c_concurrentBatchBytes> expected{};
                EncodeFrames(expectedSequence, c_concurrentBatchFrames, expected.data());
                if (output != expected)
                {
                    mismatch.store(true, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    break;
                }
                expectedSequence += c_concurrentBatchFrames;
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                abort.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });

    producer.join();
    consumer.join();
    const auto snapshot = ring.Snapshot();
    Assert(
        !abort.load(std::memory_order_relaxed) && !mismatch.load(std::memory_order_relaxed),
        "Concurrent producer and nonblocking consumer preserve every frame",
        assertions);
    Assert(
        producerDone.load(std::memory_order_acquire) && snapshot.m_framesWritten == c_concurrentTotalFrames &&
            snapshot.m_framesRead == c_concurrentTotalFrames && snapshot.m_queuedFrames == 0,
        "Concurrent transfer accounts for all accepted and consumed frames",
        assertions);
    Assert(
        snapshot.m_overflowFramesDropped % c_concurrentBatchFrames == 0, "Concurrent overflow drops only whole producer batches", assertions);
}

} // namespace

int main()
{
    try
    {
        int assertions = 0;
        TestOverflowUnderflowWholeFramesAndWrap(assertions);
        TestResetGeneration(assertions);
        TestConsumerFinalPublicationRace(assertions);
        TestConcurrentProducerConsumer(assertions);
        std::cout << "PASS: S16_LE PCM ring " << assertions
                  << " assertions; overflow/underflow, reset generations, wrap, final-publication handoff, and concurrent transfer.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
