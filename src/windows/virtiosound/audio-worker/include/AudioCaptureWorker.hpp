// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wsl::virtio_sound::audio_worker {

struct PcmFormat
{
    std::uint32_t m_sampleRate{};
    std::uint16_t m_channels{};
    std::uint16_t m_bitsPerSample{};

    bool operator==(const PcmFormat& other) const noexcept;
    bool operator!=(const PcmFormat& other) const noexcept;
};

constexpr PcmFormat RequiredCaptureFormat() noexcept
{
    return {48000, 2, 16};
}

constexpr std::size_t RequiredBytesPerFrame() noexcept
{
    return 4;
}

constexpr std::uint32_t MaximumCapturePacketFrames() noexcept
{
    return 48000;
}

enum class CaptureErrorCode
{
    None,
    InvalidEndpointId,
    Busy,
    FactoryUnavailable,
    EndpointOpenFailed,
    EndpointIsNotCapture,
    UnsupportedFormat,
    StartFailed,
    ReadFailed,
    InvalidPacket,
    BufferOverflow,
    CaptureDiscontinuity,
    StartupTimeout,
    WorkerFailed,
    StopFailed
};

struct CaptureError
{
    CaptureErrorCode m_code{CaptureErrorCode::None};
    long m_nativeCode{};
    std::wstring m_message;
};

struct CapturePacket
{
    std::uint32_t m_frames{};
    std::vector<std::uint8_t> m_s16LeBytes;
    bool m_discontinuity{};
    // Original WASAPI capture flags for metadata-only diagnostics.
    std::uint32_t m_rawFlags{};
};

enum class CaptureReadResult
{
    Packet,
    Timeout,
    Stopped,
    Failed
};

// Implementations are created and used on the worker thread. OpenEndpoint
// must use the supplied endpoint ID and return the stream format negotiated
// for that exact endpoint. ReadPacket must honor its timeout and must not
// return more than MaximumCapturePacketFrames() in one packet.
class ICaptureDevice
{
public:
    virtual ~ICaptureDevice() = default;

    virtual bool OpenEndpoint(const std::wstring& endpointId, PcmFormat& negotiatedFormat, CaptureError& error) = 0;
    virtual bool Start(CaptureError& error) = 0;
    virtual CaptureReadResult ReadPacket(
        std::chrono::milliseconds timeout, const std::atomic<bool>& stopRequested, CapturePacket& packet, CaptureError& error) = 0;
    virtual bool Close(CaptureError& error) = 0;
};

class ICaptureDeviceFactory
{
public:
    virtual ~ICaptureDeviceFactory() = default;
    virtual std::unique_ptr<ICaptureDevice> Create() = 0;
};

enum class WorkerState
{
    Stopped,
    Starting,
    Running,
    Stopping,
    Failed
};

struct WorkerSnapshot
{
    WorkerState m_state{WorkerState::Stopped};
    CaptureError m_lastError{};
    std::size_t m_queuedFrames{};
    std::size_t m_capacityFrames{};
    std::uint64_t m_framesWritten{};
    std::uint64_t m_framesRead{};
    std::uint64_t m_overflowEvents{};
    std::uint64_t m_overflowFramesDropped{};
    std::uint64_t m_discontinuities{};
    std::uint64_t m_invalidReads{};
};

// A bounded worker-owned PCM queue and lifecycle controller. Construction is
// inert. Start is the only operation that creates/opens a capture device, and
// it requires the caller to name an endpoint explicitly. The default factory
// is WASAPI; tests can inject a fake factory without touching an endpoint.
class AudioCaptureWorker final
{
public:
    static constexpr std::size_t c_defaultCapacityFrames = 96000;
    static constexpr std::chrono::milliseconds c_defaultStartupTimeout{15000};

    explicit AudioCaptureWorker(
        std::unique_ptr<ICaptureDeviceFactory> factory = {},
        std::size_t capacityFrames = c_defaultCapacityFrames,
        std::chrono::milliseconds startupTimeout = c_defaultStartupTimeout);
    ~AudioCaptureWorker();

    AudioCaptureWorker(const AudioCaptureWorker&) = delete;
    AudioCaptureWorker& operator=(const AudioCaptureWorker&) = delete;
    AudioCaptureWorker(AudioCaptureWorker&&) = delete;
    AudioCaptureWorker& operator=(AudioCaptureWorker&&) = delete;

    bool Start(const std::wstring& endpointId, CaptureError& error);
    bool Stop(CaptureError& error);

    // Does not wait for new audio, though it briefly takes the queue mutex and
    // is not realtime-safe. Returns the number of complete frames copied; zero
    // means the queue is empty or the request is invalid. destinationByteCount
    // must exactly match the requested size.
    std::size_t ReadFrames(std::uint8_t* destination, std::size_t requestedFrames, std::size_t destinationByteCount) noexcept;

    WorkerSnapshot Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Creating this factory does not initialize COM or open an endpoint. The
// returned device performs all WASAPI work only from OpenEndpoint, reached by
// AudioCaptureWorker::Start on its dedicated worker thread.
std::unique_ptr<ICaptureDeviceFactory> CreateWasapiCaptureDeviceFactory();

} // namespace wsl::virtio_sound::audio_worker
