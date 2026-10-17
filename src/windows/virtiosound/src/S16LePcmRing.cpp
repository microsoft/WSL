// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "S16LePcmRing.hpp"

namespace wsl::virtio_sound::native {

std::size_t S16LePcmRingSource::CheckedCapacityBytes(PcmFormat format, std::size_t capacityFrames)
{
    if (format.m_channels == 0 || format.m_sampleRate == 0)
    {
        throw std::invalid_argument("PCM ring format requires nonzero channels and sample rate");
    }
    if (capacityFrames == 0)
    {
        throw std::invalid_argument("PCM ring capacity must contain at least one frame");
    }

    const std::size_t frameBytes = static_cast<std::size_t>(format.m_channels) * 2;
    std::vector<std::uint8_t> maximumSizeProbe;
    if (capacityFrames > maximumSizeProbe.max_size() / frameBytes)
    {
        throw std::length_error("PCM ring capacity exceeds addressable storage");
    }
    return capacityFrames * frameBytes;
}

bool S16LePcmRingSource::ExpectedByteCount(std::size_t frames, std::size_t frameBytes, std::size_t& byteCount) noexcept
{
    if (frameBytes == 0 || frames > std::numeric_limits<std::size_t>::max() / frameBytes)
    {
        return false;
    }
    byteCount = frames * frameBytes;
    return true;
}

S16LePcmRingSource::S16LePcmRingSource(PcmFormat format, std::size_t capacityFrames) :
    m_format(format),
    m_frameBytes(static_cast<std::size_t>(format.m_channels) * 2),
    m_capacityFrames(capacityFrames),
    m_storage(CheckedCapacityBytes(format, capacityFrames))
{
}

std::uint64_t S16LePcmRingSource::Generation() const noexcept
{
    return m_generation.load(std::memory_order_acquire);
}

PcmRingWriteResult S16LePcmRingSource::TryWriteFrames(std::uint64_t expectedGeneration, const std::uint8_t* source, std::size_t frames, std::size_t byteCount) noexcept
{
    if (frames == 0 && byteCount == 0)
    {
        return PcmRingWriteResult::EmptyWrite;
    }

    std::size_t expectedBytes = 0;
    if (source == nullptr || !ExpectedByteCount(frames, m_frameBytes, expectedBytes) || expectedBytes != byteCount)
    {
        m_invalidWrites.fetch_add(1, std::memory_order_relaxed);
        return PcmRingWriteResult::InvalidBuffer;
    }

    if (m_resetFaulted.load(std::memory_order_acquire))
    {
        return PcmRingWriteResult::ResetFaulted;
    }

    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_resetFaulted.load(std::memory_order_relaxed))
        {
            return PcmRingWriteResult::ResetFaulted;
        }
        if (m_generationExhausted || expectedGeneration != m_generation.load(std::memory_order_relaxed))
        {
            m_staleGenerationWrites.fetch_add(1, std::memory_order_relaxed);
            return PcmRingWriteResult::StaleGeneration;
        }
        const std::uint64_t readIndex = m_readFrameIndex.load(std::memory_order_acquire);
        const std::uint64_t queued = m_writeFrameIndex - readIndex;
        if (queued > m_capacityFrames)
        {
            m_sequenceFailures.fetch_add(1, std::memory_order_relaxed);
            m_resetFaulted.store(true, std::memory_order_release);
            return PcmRingWriteResult::ResetFaulted;
        }
        const std::size_t queuedFrames = static_cast<std::size_t>(queued);
        if (frames > m_capacityFrames - queuedFrames)
        {
            m_overflowEvents.fetch_add(1, std::memory_order_relaxed);
            m_overflowFramesDropped.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
            return PcmRingWriteResult::Overflow;
        }
        if (static_cast<std::uint64_t>(frames) > std::numeric_limits<std::uint64_t>::max() - m_writeFrameIndex)
        {
            m_sequenceFailures.fetch_add(1, std::memory_order_relaxed);
            m_resetFaulted.store(true, std::memory_order_release);
            return PcmRingWriteResult::ResetFaulted;
        }

        const std::size_t writeOffset = static_cast<std::size_t>(m_writeFrameIndex % m_capacityFrames);
        const std::size_t firstFrames = std::min(frames, m_capacityFrames - writeOffset);
        const std::size_t firstBytes = firstFrames * m_frameBytes;
        std::memcpy(m_storage.data() + writeOffset * m_frameBytes, source, firstBytes);
        const std::size_t secondBytes = byteCount - firstBytes;
        if (secondBytes != 0)
        {
            std::memcpy(m_storage.data(), source + firstBytes, secondBytes);
        }

        m_writeFrameIndex += static_cast<std::uint64_t>(frames);
        m_publishedWriteFrameIndex.store(m_writeFrameIndex, std::memory_order_release);
        m_framesWritten.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
        return PcmRingWriteResult::Written;
    }
    catch (...)
    {
        m_writeLockFailures.fetch_add(1, std::memory_order_relaxed);
        return PcmRingWriteResult::SynchronizationFailure;
    }
}

bool S16LePcmRingSource::ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept
{
    if (frames == 0 && byteCount == 0)
    {
        return true;
    }

    std::size_t expectedBytes = 0;
    if (destination == nullptr || !ExpectedByteCount(frames, m_frameBytes, expectedBytes) || expectedBytes != byteCount)
    {
        m_invalidReads.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (m_resetFaulted.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::uint64_t readIndex = m_readFrameIndex.load(std::memory_order_relaxed);
    const std::uint64_t writeIndex = m_publishedWriteFrameIndex.load(std::memory_order_acquire);
    const std::uint64_t queued = writeIndex - readIndex;
    if (queued > m_capacityFrames)
    {
        m_sequenceFailures.fetch_add(1, std::memory_order_relaxed);
        m_resetFaulted.store(true, std::memory_order_release);
        return false;
    }
    const std::size_t queuedFrames = static_cast<std::size_t>(queued);
    if (static_cast<std::size_t>(frames) > queuedFrames)
    {
        m_underflowEvents.fetch_add(1, std::memory_order_relaxed);
        m_underflowFramesMissing.fetch_add(static_cast<std::uint64_t>(static_cast<std::size_t>(frames) - queuedFrames), std::memory_order_relaxed);
        return false;
    }

    const std::size_t readOffset = static_cast<std::size_t>(readIndex % m_capacityFrames);
    const std::size_t firstFrames = std::min(static_cast<std::size_t>(frames), m_capacityFrames - readOffset);
    const std::size_t firstBytes = firstFrames * m_frameBytes;
    std::memcpy(destination, m_storage.data() + readOffset * m_frameBytes, firstBytes);
    const std::size_t secondBytes = byteCount - firstBytes;
    if (secondBytes != 0)
    {
        std::memcpy(destination + firstBytes, m_storage.data(), secondBytes);
    }

    m_readFrameIndex.store(readIndex + static_cast<std::uint64_t>(frames), std::memory_order_release);
    m_framesRead.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

void S16LePcmRingSource::Reset() noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readFrameIndex.store(0, std::memory_order_release);
        m_writeFrameIndex = 0;
        m_publishedWriteFrameIndex.store(0, std::memory_order_release);

        std::uint64_t current = m_generation.load(std::memory_order_relaxed);
        if (current == std::numeric_limits<std::uint64_t>::max())
        {
            // Never let an ancient token become current again on wraparound.
            m_generationExhausted = true;
        }
        else
        {
            ++current;
            m_generation.store(current, std::memory_order_release);
        }
        m_resetFaulted.store(false, std::memory_order_release);
        m_resetCount.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
        m_resetFaulted.store(true, std::memory_order_release);
        m_resetFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

PcmRingSnapshot S16LePcmRingSource::Snapshot() const noexcept
{
    const std::uint64_t readIndex = m_readFrameIndex.load(std::memory_order_acquire);
    const std::uint64_t writeIndex = m_publishedWriteFrameIndex.load(std::memory_order_acquire);
    const std::uint64_t queued = writeIndex - readIndex;
    return PcmRingSnapshot{
        m_generation.load(std::memory_order_acquire),
        queued <= m_capacityFrames ? static_cast<std::size_t>(queued) : 0,
        m_resetCount.load(std::memory_order_relaxed),
        m_framesWritten.load(std::memory_order_relaxed),
        m_framesRead.load(std::memory_order_relaxed),
        m_overflowEvents.load(std::memory_order_relaxed),
        m_overflowFramesDropped.load(std::memory_order_relaxed),
        m_underflowEvents.load(std::memory_order_relaxed),
        m_underflowFramesMissing.load(std::memory_order_relaxed),
        m_staleGenerationWrites.load(std::memory_order_relaxed),
        m_invalidWrites.load(std::memory_order_relaxed),
        m_invalidReads.load(std::memory_order_relaxed),
        m_sequenceFailures.load(std::memory_order_relaxed),
        m_writeLockFailures.load(std::memory_order_relaxed),
        m_resetFailures.load(std::memory_order_relaxed)};
}

} // namespace wsl::virtio_sound::native
