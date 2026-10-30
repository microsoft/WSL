// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "VirtioSoundDevice.hpp"

namespace wsl::virtio_sound::native {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "The PCM ring consumer requires lock-free 64-bit atomics");

enum class PcmRingWriteResult
{
    Written,
    EmptyWrite,
    StaleGeneration,
    InvalidBuffer,
    Overflow,
    ResetFaulted,
    SynchronizationFailure
};

struct PcmRingSnapshot
{
    std::uint64_t m_generation{};
    std::size_t m_queuedFrames{};
    std::uint64_t m_resetCount{};
    std::uint64_t m_framesWritten{};
    std::uint64_t m_framesRead{};
    std::uint64_t m_overflowEvents{};
    std::uint64_t m_overflowFramesDropped{};
    std::uint64_t m_underflowEvents{};
    std::uint64_t m_underflowFramesMissing{};
    std::uint64_t m_staleGenerationWrites{};
    std::uint64_t m_invalidWrites{};
    std::uint64_t m_invalidReads{};
    std::uint64_t m_sequenceFailures{};
    std::uint64_t m_writeLockFailures{};
    std::uint64_t m_resetFailures{};
};

// Fixed-capacity interleaved S16_LE ring source. There is one logical producer
// (TryWriteFrames calls serialize on m_mutex) and one consumer. The producer
// supplies a generation token captured before producing its PCM; Reset
// invalidates old tokens, so a producer racing a stream reset cannot enqueue
// stale samples. Writes are all-or-nothing and reject the newest batch on
// overflow. The owner must serialize ReadFrames with Reset; VirtioSoundDevice
// already does so under its device gate.
class S16LePcmRingSource final : public PcmSource
{
public:
    S16LePcmRingSource(PcmFormat format, std::size_t capacityFrames);

    S16LePcmRingSource(const S16LePcmRingSource&) = delete;
    S16LePcmRingSource& operator=(const S16LePcmRingSource&) = delete;

    std::uint64_t Generation() const noexcept;
    PcmRingWriteResult TryWriteFrames(std::uint64_t expectedGeneration, const std::uint8_t* source, std::size_t frames, std::size_t byteCount) noexcept;
    PcmRingSnapshot Snapshot() const noexcept;

    // Lock-free single-consumer path: no mutex, allocation, or wait. Underflow
    // and invalid requests return false without consuming or modifying
    // destination data. Concurrent ReadFrames calls are unsupported.
    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept override;

    // Cumulative counters survive reset; queued samples and cursors do not.
    void Reset() noexcept override;

    PcmFormat Format() const noexcept
    {
        return m_format;
    }
    std::size_t CapacityFrames() const noexcept
    {
        return m_capacityFrames;
    }
    std::size_t BytesPerFrame() const noexcept
    {
        return m_frameBytes;
    }

private:
    static std::size_t CheckedCapacityBytes(PcmFormat format, std::size_t capacityFrames);
    static bool ExpectedByteCount(std::size_t frames, std::size_t frameBytes, std::size_t& byteCount) noexcept;

    PcmFormat m_format{};
    std::size_t m_frameBytes{};
    std::size_t m_capacityFrames{};
    std::vector<std::uint8_t> m_storage;
    // The standalone portable target cannot depend on Windows-only WIL locks.
    mutable std::mutex m_mutex;
    std::atomic<std::uint64_t> m_readFrameIndex{};
    std::uint64_t m_writeFrameIndex{}; // protected by m_mutex
    std::atomic<std::uint64_t> m_publishedWriteFrameIndex{};
    bool m_generationExhausted{};
    std::atomic<std::uint64_t> m_generation{1};
    std::atomic<bool> m_resetFaulted{};

    std::atomic<std::uint64_t> m_resetCount{};
    std::atomic<std::uint64_t> m_framesWritten{};
    std::atomic<std::uint64_t> m_framesRead{};
    std::atomic<std::uint64_t> m_overflowEvents{};
    std::atomic<std::uint64_t> m_overflowFramesDropped{};
    std::atomic<std::uint64_t> m_underflowEvents{};
    std::atomic<std::uint64_t> m_underflowFramesMissing{};
    std::atomic<std::uint64_t> m_staleGenerationWrites{};
    std::atomic<std::uint64_t> m_invalidWrites{};
    std::atomic<std::uint64_t> m_invalidReads{};
    std::atomic<std::uint64_t> m_sequenceFailures{};
    std::atomic<std::uint64_t> m_writeLockFailures{};
    std::atomic<std::uint64_t> m_resetFailures{};
};

} // namespace wsl::virtio_sound::native
