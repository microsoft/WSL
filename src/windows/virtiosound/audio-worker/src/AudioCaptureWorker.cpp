// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "AudioCaptureWorker.hpp"
#include "AudioWorkerTrace.hpp"

namespace wsl::virtio_sound::audio_worker {
namespace {

    constexpr auto c_capturePollInterval = std::chrono::milliseconds(20);

    CaptureError MakeError(CaptureErrorCode code, const wchar_t* message)
    {
        return {code, 0, message};
    }

    CaptureError NormalizeError(CaptureError error, CaptureErrorCode fallback, const wchar_t* message)
    {
        if (error.m_code == CaptureErrorCode::None)
        {
            error.m_code = fallback;
            if (error.m_message.empty())
            {
                error.m_message = message;
            }
        }
        return error;
    }

    bool IsBlank(const std::wstring& value)
    {
        return value.empty() || std::all_of(value.begin(), value.end(), [](wchar_t character) {
                   return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n';
               });
    }

    enum class QueueWriteResult
    {
        Written,
        Overflow,
        InvalidPacket
    };

    class BoundedPcmQueue final
    {
    public:
        explicit BoundedPcmQueue(std::size_t capacityFrames) : m_capacityFrames(capacityFrames)
        {
            if (m_capacityFrames == 0 || m_capacityFrames > (std::numeric_limits<std::size_t>::max)() / RequiredBytesPerFrame())
            {
                throw std::invalid_argument("PCM queue capacity is invalid or too large.");
            }
            m_storage.resize(m_capacityFrames * RequiredBytesPerFrame());
        }

        void Reset() noexcept
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_headFrame = 0;
            m_queuedFrames = 0;
        }

        QueueWriteResult TryWrite(const CapturePacket& packet) noexcept
        {
            const std::size_t frameBytes = RequiredBytesPerFrame();
            if (packet.m_frames == 0 || packet.m_frames > MaximumCapturePacketFrames() ||
                static_cast<std::size_t>(packet.m_frames) > (std::numeric_limits<std::size_t>::max)() / frameBytes ||
                packet.m_s16LeBytes.size() != static_cast<std::size_t>(packet.m_frames) * frameBytes)
            {
                return QueueWriteResult::InvalidPacket;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (static_cast<std::size_t>(packet.m_frames) > m_capacityFrames - m_queuedFrames)
            {
                ++m_overflowEvents;
                m_overflowFramesDropped += packet.m_frames;
                m_headFrame = 0;
                m_queuedFrames = 0;
                return QueueWriteResult::Overflow;
            }

            const std::size_t writeFrame = (m_headFrame + m_queuedFrames) % m_capacityFrames;
            CopyIntoRing(writeFrame, packet.m_s16LeBytes.data(), packet.m_frames);
            m_queuedFrames += packet.m_frames;
            m_framesWritten += packet.m_frames;
            return QueueWriteResult::Written;
        }

        void DiscardForDiscontinuity() noexcept
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_discontinuities;
            m_headFrame = 0;
            m_queuedFrames = 0;
        }

        std::size_t Read(std::uint8_t* destination, std::size_t requestedFrames, std::size_t destinationByteCount) noexcept
        {
            if (requestedFrames == 0)
            {
                return destinationByteCount == 0 ? 0 : RecordInvalidRead();
            }
            if (destination == nullptr || requestedFrames > (std::numeric_limits<std::size_t>::max)() / RequiredBytesPerFrame() ||
                destinationByteCount != requestedFrames * RequiredBytesPerFrame())
            {
                return RecordInvalidRead();
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            const std::size_t framesToRead = (std::min)(requestedFrames, m_queuedFrames);
            if (framesToRead == 0)
            {
                return 0;
            }
            CopyOutOfRing(m_headFrame, destination, framesToRead);
            m_headFrame = (m_headFrame + framesToRead) % m_capacityFrames;
            m_queuedFrames -= framesToRead;
            m_framesRead += framesToRead;
            return framesToRead;
        }

        WorkerSnapshot AddToSnapshot(WorkerSnapshot snapshot) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot.m_queuedFrames = m_queuedFrames;
            snapshot.m_capacityFrames = m_capacityFrames;
            snapshot.m_framesWritten = m_framesWritten;
            snapshot.m_framesRead = m_framesRead;
            snapshot.m_overflowEvents = m_overflowEvents;
            snapshot.m_overflowFramesDropped = m_overflowFramesDropped;
            snapshot.m_discontinuities = m_discontinuities;
            snapshot.m_invalidReads = m_invalidReads;
            return snapshot;
        }

    private:
        std::size_t RecordInvalidRead() noexcept
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_invalidReads;
            return 0;
        }

        void CopyIntoRing(std::size_t destinationFrame, const std::uint8_t* source, std::size_t frameCount) noexcept
        {
            const std::size_t firstFrames = (std::min)(frameCount, m_capacityFrames - destinationFrame);
            const std::size_t frameBytes = RequiredBytesPerFrame();
            std::memcpy(m_storage.data() + destinationFrame * frameBytes, source, firstFrames * frameBytes);
            if (firstFrames != frameCount)
            {
                std::memcpy(m_storage.data(), source + firstFrames * frameBytes, (frameCount - firstFrames) * frameBytes);
            }
        }

        void CopyOutOfRing(std::size_t sourceFrame, std::uint8_t* destination, std::size_t frameCount) const noexcept
        {
            const std::size_t firstFrames = (std::min)(frameCount, m_capacityFrames - sourceFrame);
            const std::size_t frameBytes = RequiredBytesPerFrame();
            std::memcpy(destination, m_storage.data() + sourceFrame * frameBytes, firstFrames * frameBytes);
            if (firstFrames != frameCount)
            {
                std::memcpy(destination + firstFrames * frameBytes, m_storage.data(), (frameCount - firstFrames) * frameBytes);
            }
        }

        const std::size_t m_capacityFrames;
        mutable std::mutex m_mutex;
        std::vector<std::uint8_t> m_storage;
        std::size_t m_headFrame{};
        std::size_t m_queuedFrames{};
        std::uint64_t m_framesWritten{};
        std::uint64_t m_framesRead{};
        std::uint64_t m_overflowEvents{};
        std::uint64_t m_overflowFramesDropped{};
        std::uint64_t m_discontinuities{};
        std::uint64_t m_invalidReads{};
    };

} // namespace

bool PcmFormat::operator==(const PcmFormat& other) const noexcept
{
    return m_sampleRate == other.m_sampleRate && m_channels == other.m_channels && m_bitsPerSample == other.m_bitsPerSample;
}

bool PcmFormat::operator!=(const PcmFormat& other) const noexcept
{
    return !(*this == other);
}

class AudioCaptureWorker::Impl final
{
public:
    Impl(std::unique_ptr<ICaptureDeviceFactory> factory, std::size_t capacityFrames, std::chrono::milliseconds startupTimeout) :
        m_factory(factory ? std::move(factory) : CreateWasapiCaptureDeviceFactory()), m_queue(capacityFrames), m_startupTimeout(startupTimeout)
    {
        if (m_startupTimeout <= std::chrono::milliseconds::zero())
        {
            throw std::invalid_argument("Capture startup timeout must be positive.");
        }
    }

    ~Impl()
    {
        CaptureError ignored;
        (void)Stop(ignored);
    }

    bool Start(const std::wstring& endpointId, CaptureError& error)
    {
        std::lock_guard<std::mutex> controlLock(m_controlMutex);
        error = {};
        if (m_worker.joinable())
        {
            error = MakeError(CaptureErrorCode::Busy, L"The capture worker must be stopped before it can be started again.");
            return false;
        }
        if (IsBlank(endpointId))
        {
            error = MakeError(CaptureErrorCode::InvalidEndpointId, L"An explicit non-empty capture endpoint ID is required.");
            SetLastError(error, WorkerState::Stopped);
            return false;
        }
        if (!m_factory)
        {
            error = MakeError(CaptureErrorCode::FactoryUnavailable, L"No capture-device factory is available.");
            SetLastError(error, WorkerState::Stopped);
            return false;
        }

        m_queue.Reset();
        m_stopRequested.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_state = WorkerState::Starting;
            m_startupComplete = false;
            m_lastError = {};
        }

        try
        {
            m_worker = std::thread(&Impl::WorkerMain, this, endpointId);
        }
        catch (const std::exception&)
        {
            error = MakeError(CaptureErrorCode::WorkerFailed, L"The capture worker thread could not be created.");
            SetLastError(error, WorkerState::Stopped);
            return false;
        }

        std::unique_lock<std::mutex> stateLock(m_stateMutex);
        const bool startupComplete = m_startupCondition.wait_for(stateLock, m_startupTimeout, [this] { return m_startupComplete; });
        if (!startupComplete)
        {
            error = MakeError(CaptureErrorCode::StartupTimeout, L"Capture endpoint startup exceeded its configured deadline.");
            m_lastError = error;
            m_state = WorkerState::Stopping;
            m_stopRequested.store(true, std::memory_order_release);
            stateLock.unlock();
            m_worker.join();
            m_queue.Reset();
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_state = WorkerState::Stopped;
                m_lastError = error;
            }
            return false;
        }

        if (m_state == WorkerState::Running)
        {
            return true;
        }

        error = m_lastError;
        if (error.m_code == CaptureErrorCode::None)
        {
            error = MakeError(CaptureErrorCode::WorkerFailed, L"The capture worker stopped before it became ready.");
        }
        stateLock.unlock();
        m_worker.join();
        m_queue.Reset();
        SetState(WorkerState::Stopped);
        return false;
    }

    bool Stop(CaptureError& error)
    {
        std::lock_guard<std::mutex> controlLock(m_controlMutex);
        error = {};
        if (!m_worker.joinable())
        {
            m_queue.Reset();
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_state != WorkerState::Stopped)
            {
                m_state = WorkerState::Stopped;
            }
            return true;
        }

        m_stopRequested.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_state == WorkerState::Running || m_state == WorkerState::Starting)
            {
                m_state = WorkerState::Stopping;
            }
        }
        m_worker.join();
        m_queue.Reset();

        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (m_state != WorkerState::Failed)
        {
            m_state = WorkerState::Stopped;
        }
        if (m_lastError.m_code != CaptureErrorCode::None)
        {
            error = m_lastError;
            return false;
        }
        return true;
    }

    std::size_t ReadFrames(std::uint8_t* destination, std::size_t requestedFrames, std::size_t destinationByteCount) noexcept
    {
        return m_queue.Read(destination, requestedFrames, destinationByteCount);
    }

    WorkerSnapshot Snapshot() const
    {
        WorkerSnapshot snapshot;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            snapshot.m_state = m_state;
            snapshot.m_lastError = m_lastError;
        }
        return m_queue.AddToSnapshot(std::move(snapshot));
    }

private:
    void SetLastError(const CaptureError& error, WorkerState state)
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_lastError = error;
        m_state = state;
        m_startupComplete = true;
        m_startupCondition.notify_all();
    }

    void SetState(WorkerState state)
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_state = state;
    }

    void CompleteStartup(WorkerState state, const CaptureError& error)
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_state = state;
        if (error.m_code != CaptureErrorCode::None)
        {
            m_lastError = error;
        }
        m_startupComplete = true;
        m_startupCondition.notify_all();
    }

    void Fail(CaptureError error, CaptureErrorCode fallback, const wchar_t* message)
    {
        error = NormalizeError(std::move(error), fallback, message);
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_lastError = std::move(error);
        m_state = WorkerState::Failed;
        m_startupComplete = true;
        m_startupCondition.notify_all();
    }

    void WorkerMain(const std::wstring& endpointId) noexcept
    {
        std::unique_ptr<ICaptureDevice> device;
        CaptureError workerError;
        bool failed = false;
        bool firstPacketLogged = false;
        try
        {
            device = m_factory->Create();
            if (!device)
            {
                workerError =
                    MakeError(CaptureErrorCode::FactoryUnavailable, L"The capture-device factory did not create a device.");
                failed = true;
                Fail(workerError, CaptureErrorCode::FactoryUnavailable, L"The capture-device factory did not create a device.");
            }
            else
            {
                PcmFormat negotiatedFormat{};
                if (!device->OpenEndpoint(endpointId, negotiatedFormat, workerError))
                {
                    workerError = NormalizeError(
                        std::move(workerError),
                        CaptureErrorCode::EndpointOpenFailed,
                        L"The selected capture endpoint could not be opened.");
                    failed = true;
                    Fail(workerError, CaptureErrorCode::EndpointOpenFailed, L"The selected capture endpoint could not be opened.");
                }
                else
                {
                    if (negotiatedFormat != RequiredCaptureFormat())
                    {
                        const std::uint64_t formatDetails =
                            (static_cast<std::uint64_t>(negotiatedFormat.m_channels) << 16) | negotiatedFormat.m_bitsPerSample;
                        detail::TraceAudioWorker(L"UnsupportedFormat", negotiatedFormat.m_sampleRate, formatDetails);
                        workerError = MakeError(
                            CaptureErrorCode::UnsupportedFormat, L"The selected endpoint did not negotiate 48 kHz stereo S16LE.");
                        failed = true;
                        Fail(
                            workerError,
                            CaptureErrorCode::UnsupportedFormat,
                            L"The selected endpoint did not negotiate 48 kHz stereo S16LE.");
                    }
                    else if (m_stopRequested.load(std::memory_order_acquire))
                    {
                        CompleteStartup(WorkerState::Stopped, {});
                    }
                    else if (!device->Start(workerError))
                    {
                        workerError = NormalizeError(
                            std::move(workerError),
                            CaptureErrorCode::StartFailed,
                            L"The selected capture endpoint could not be started.");
                        failed = true;
                        Fail(workerError, CaptureErrorCode::StartFailed, L"The selected capture endpoint could not be started.");
                    }
                    else
                    {
                        CompleteStartup(WorkerState::Running, {});
                        while (!m_stopRequested.load(std::memory_order_acquire))
                        {
                            CapturePacket packet;
                            workerError = {};
                            const auto result = device->ReadPacket(c_capturePollInterval, m_stopRequested, packet, workerError);
                            if (result == CaptureReadResult::Packet && !firstPacketLogged)
                            {
                                detail::TraceAudioWorker(L"FirstPacket", packet.m_rawFlags, packet.m_frames);
                                firstPacketLogged = true;
                            }
                            if (result == CaptureReadResult::Timeout)
                            {
                                continue;
                            }
                            if (result == CaptureReadResult::Stopped)
                            {
                                if (!m_stopRequested.load(std::memory_order_acquire))
                                {
                                    workerError =
                                        MakeError(CaptureErrorCode::ReadFailed, L"The capture device stopped unexpectedly.");
                                    failed = true;
                                    Fail(workerError, CaptureErrorCode::ReadFailed, L"The capture device stopped unexpectedly.");
                                }
                                break;
                            }
                            if (result == CaptureReadResult::Failed)
                            {
                                workerError = NormalizeError(
                                    std::move(workerError), CaptureErrorCode::ReadFailed, L"Reading a capture packet failed.");
                                failed = true;
                                Fail(workerError, CaptureErrorCode::ReadFailed, L"Reading a capture packet failed.");
                                break;
                            }
                            if (packet.m_frames == 0 || packet.m_frames > MaximumCapturePacketFrames())
                            {
                                workerError = MakeError(
                                    CaptureErrorCode::InvalidPacket,
                                    L"The capture device returned an invalid or oversized PCM packet.");
                                failed = true;
                                Fail(
                                    workerError,
                                    CaptureErrorCode::InvalidPacket,
                                    L"The capture device returned an invalid or oversized PCM packet.");
                                break;
                            }
                            if (packet.m_discontinuity)
                            {
                                detail::TraceAudioWorker(L"DiscontinuityPacket", packet.m_rawFlags, packet.m_frames);
                                m_queue.DiscardForDiscontinuity();
                                workerError = MakeError(
                                    CaptureErrorCode::CaptureDiscontinuity,
                                    L"The capture endpoint reported a discontinuity; buffered PCM was discarded.");
                                failed = true;
                                Fail(
                                    workerError,
                                    CaptureErrorCode::CaptureDiscontinuity,
                                    L"The capture endpoint reported a discontinuity.");
                                break;
                            }
                            const QueueWriteResult writeResult = m_queue.TryWrite(packet);
                            if (writeResult == QueueWriteResult::InvalidPacket)
                            {
                                workerError = MakeError(
                                    CaptureErrorCode::InvalidPacket, L"The capture device returned an invalid S16LE packet size.");
                                failed = true;
                                Fail(
                                    workerError,
                                    CaptureErrorCode::InvalidPacket,
                                    L"The capture device returned an invalid S16LE packet size.");
                                break;
                            }
                            if (writeResult == QueueWriteResult::Overflow)
                            {
                                workerError = MakeError(
                                    CaptureErrorCode::BufferOverflow,
                                    L"The bounded PCM queue overflowed; capture stopped to avoid an unreported audio gap.");
                                failed = true;
                                Fail(workerError, CaptureErrorCode::BufferOverflow, L"The bounded PCM queue overflowed.");
                                break;
                            }
                        }
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            detail::TraceAudioWorker(L"WorkerException", 1, static_cast<std::uint64_t>(CaptureErrorCode::WorkerFailed));
            workerError = MakeError(CaptureErrorCode::WorkerFailed, L"An exception interrupted the capture worker.");
            failed = true;
            Fail(workerError, CaptureErrorCode::WorkerFailed, L"An exception interrupted the capture worker.");
        }
        catch (...)
        {
            detail::TraceAudioWorker(L"WorkerException", 2, static_cast<std::uint64_t>(CaptureErrorCode::WorkerFailed));
            workerError = MakeError(CaptureErrorCode::WorkerFailed, L"An unknown error interrupted the capture worker.");
            failed = true;
            Fail(workerError, CaptureErrorCode::WorkerFailed, L"An unknown error interrupted the capture worker.");
        }

        if (device)
        {
            try
            {
                CaptureError closeError;
                if (!device->Close(closeError) && !failed)
                {
                    workerError = NormalizeError(
                        std::move(closeError), CaptureErrorCode::StopFailed, L"The capture endpoint did not close cleanly.");
                    failed = true;
                }
            }
            catch (...)
            {
                detail::TraceAudioWorker(L"WorkerException", 3, static_cast<std::uint64_t>(CaptureErrorCode::StopFailed));
                if (!failed)
                {
                    workerError = MakeError(CaptureErrorCode::StopFailed, L"An exception interrupted capture endpoint shutdown.");
                    failed = true;
                }
            }
        }

        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (failed)
        {
            if (m_lastError.m_code == CaptureErrorCode::None)
            {
                m_lastError = workerError;
            }
            m_state = WorkerState::Failed;
        }
        else
        {
            m_state = WorkerState::Stopped;
        }
        m_startupComplete = true;
        m_startupCondition.notify_all();
    }

    std::unique_ptr<ICaptureDeviceFactory> m_factory;
    BoundedPcmQueue m_queue;
    const std::chrono::milliseconds m_startupTimeout;
    std::atomic<bool> m_stopRequested{};
    // Keep the worker's state lock in the standard C++ synchronization model with its condition variable.
    mutable std::mutex m_stateMutex;
    std::condition_variable m_startupCondition;
    std::mutex m_controlMutex;
    std::thread m_worker;
    WorkerState m_state{WorkerState::Stopped};
    CaptureError m_lastError{};
    bool m_startupComplete{};
};

AudioCaptureWorker::AudioCaptureWorker(std::unique_ptr<ICaptureDeviceFactory> factory, std::size_t capacityFrames, std::chrono::milliseconds startupTimeout) :
    m_impl(std::make_unique<Impl>(std::move(factory), capacityFrames, startupTimeout))
{
}

AudioCaptureWorker::~AudioCaptureWorker() = default;

bool AudioCaptureWorker::Start(const std::wstring& endpointId, CaptureError& error)
{
    return m_impl->Start(endpointId, error);
}

bool AudioCaptureWorker::Stop(CaptureError& error)
{
    return m_impl->Stop(error);
}

std::size_t AudioCaptureWorker::ReadFrames(std::uint8_t* destination, std::size_t requestedFrames, std::size_t destinationByteCount) noexcept
{
    return m_impl->ReadFrames(destination, requestedFrames, destinationByteCount);
}

WorkerSnapshot AudioCaptureWorker::Snapshot() const
{
    return m_impl->Snapshot();
}

} // namespace wsl::virtio_sound::audio_worker
