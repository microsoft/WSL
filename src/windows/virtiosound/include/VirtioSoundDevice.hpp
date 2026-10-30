// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace wsl::virtio_sound::native {

// Callbacks must remain valid for the lifetime of VirtioSoundDevice. A false
// return is treated as a guest-memory or interrupt transport failure. No
// Windows, HCS, HDV, or audio headers are needed by the portable core.
struct GuestMemoryCallbacks
{
    void* m_context{};
    bool (*m_read)(void* context, std::uint64_t guestPhysicalAddress, std::uint8_t* destination, std::size_t byteCount){};
    bool (*m_write)(void* context, std::uint64_t guestPhysicalAddress, const std::uint8_t* source, std::size_t byteCount){};
};

struct InterruptCallbacks
{
    void* m_context{};
    bool (*m_deliverMsi)(void* context, std::uint64_t address, std::uint32_t data){};
    bool (*m_setIntx)(void* context, bool asserted){};
};

struct PcmFormat
{
    std::uint8_t m_channels{2};
    std::uint32_t m_sampleRate{48000};
};

enum class SourceReadiness : std::uint8_t
{
    Pending,
    Ready,
    Failed
};

// An external source returns exactly frames * channels * 2 bytes of
// interleaved S16_LE PCM. Reset discards queued host samples. The device calls
// the lifecycle and readiness methods while holding its gate, so they must be
// bounded state notifications/queries. They may synchronize with bounded
// local queue state, but must not perform capture, pipe, or other external I/O,
// wait for a worker, or reenter VirtioSoundDevice.
// Implementations must not throw across this interface; false from
// StartCapture/ReadFrames reports an I/O error.
class PcmSource
{
public:
    virtual ~PcmSource() = default;
    virtual bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept = 0;
    virtual void Reset() noexcept = 0;

    // Start/stop preserve compatibility with simple synchronous sources by
    // mapping to Reset. Asynchronous sources override these with state-only
    // notifications; StartCapture receives the negotiated period in frames.
    virtual bool StartCapture(std::uint32_t periodFrames) noexcept
    {
        (void)periodFrames;
        Reset();
        return true;
    }
    virtual void StopCapture() noexcept
    {
        Reset();
    }
    virtual SourceReadiness CaptureReadiness() const noexcept
    {
        return SourceReadiness::Ready;
    }

    // Sources paced by an external clock may expose the number of frames
    // already buffered for immediate delivery. nullopt keeps the device's
    // elapsed-time pacing for sources whose clock is unknown to the device.
    // Whether this returns nullopt must remain stable across stream states,
    // including before SET_PARAMS. This is also used to validate pump capacity.
    // This query must remain a bounded state-only operation.
    virtual std::optional<std::uint32_t> AvailableFrames() const noexcept
    {
        return std::nullopt;
    }
};

struct DiagnosticSnapshot
{
    bool m_rxQueueEnabled{};
    std::uint16_t m_rxLastAvailableIndex{};
    std::uint16_t m_rxObservedAvailableIndex{};
    std::uint16_t m_rxOutstandingAvailable{};
    std::uint16_t m_rxNextUsedIndex{};
    std::uint64_t m_resetCount{};
    std::uint64_t m_rxAvailableProcessed{};
    std::uint64_t m_rxUsedEntriesPublished{};
    std::uint64_t m_rxDataCompletions{};
    std::uint64_t m_rxStatusOnlyCompletions{};
    std::uint64_t m_notificationRequests{};
    std::uint64_t m_notificationFailures{};
    std::uint64_t m_interruptSignalsRequested{};
    std::uint64_t m_interruptApiRequests{};
    std::uint64_t m_interruptApiSuccesses{};
    std::uint64_t m_interruptApiFailures{};
};

class VirtioSoundDevice final
{
public:
    static constexpr std::uint16_t c_vendorId = 0x1af4;
    static constexpr std::uint16_t c_deviceId = 0x1059;
    static constexpr std::uint32_t c_barSize = 8192;
    static constexpr std::uint32_t c_queueCount = 4;
    static constexpr std::uint16_t c_maxQueueSize = 128;
    static constexpr std::uint32_t c_maxTransferBytes = 1024 * 1024;
    static constexpr std::uint32_t c_capturePumpIntervalMs = 10;

    // A null PCM source selects the deterministic stereo 48 kHz S16 ramp.
    // Any other format requires an explicit source.
    VirtioSoundDevice(GuestMemoryCallbacks guestMemory, InterruptCallbacks interrupts, std::shared_ptr<PcmSource> pcmSource = {}, PcmFormat format = {});
    ~VirtioSoundDevice();

    VirtioSoundDevice(const VirtioSoundDevice&) = delete;
    VirtioSoundDevice& operator=(const VirtioSoundDevice&) = delete;

    std::uint32_t ReadConfig(std::uint32_t offset, std::uint8_t width = 4);
    void WriteConfig(std::uint32_t offset, std::uint32_t value, std::uint8_t width = 4);
    void ReadBar(std::uint64_t offset, std::uint8_t* destination, std::size_t byteCount);
    void WriteBar(std::uint64_t offset, const std::uint8_t* source, std::size_t byteCount);

    int ProcessControlQueue();
    int ProcessEventQueue();
    // Called by the owning service's clock/pump. Guest RX kicks never grant a
    // frame budget, so capture remains paced by the service.
    int PumpCapture(int frameBudget);
    // Accumulates elapsed clock time under the device lock, clears stale
    // credit on stream transitions, pumps within the transport bound, then
    // subtracts completed frames. One changed generation is observed without
    // crediting the preceding interval because its active duration is unknown.
    int PumpCaptureElapsed(std::uint64_t elapsedNanoseconds);
    bool CaptureRunning() const noexcept;
    std::uint32_t CapturePeriodFrames() const noexcept;
    std::uint32_t MaximumCaptureFrameBudget() const noexcept;
    std::uint64_t CapturePacingGeneration() const noexcept;
    void Reset();

    std::uint8_t DeviceStatus() const;
    std::string LastFault() const;
    DiagnosticSnapshot GetDiagnosticsSnapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wsl::virtio_sound::native
