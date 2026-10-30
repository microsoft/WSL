// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "AudioCaptureWorker.hpp"
#include "AudioIpcCodec.hpp"

namespace wsl::virtio_sound::audio_worker {

// These values are the stable session protocol fault codes. They intentionally
// do not reuse CaptureErrorCode's numeric values.
enum class AudioWorkerFaultCode : std::uint32_t
{
    ProtocolViolation = 1U,
    SequenceViolation = 2U,
    InvalidSessionState = 3U,
    InvalidEndpoint = 4U,
    CaptureStartFailed = 5U,
    CaptureReadFailed = 6U,
    CaptureStopFailed = 7U,
    CaptureDiscontinuity = 8U,
    CaptureQueueOverflow = 9U,
    InvalidCapturePacket = 10U,
    CaptureStartupTimeout = 11U,
    CaptureUnavailable = 12U,
    OutboundSequenceExhausted = 13U,
    InternalFailure = 14U,
};

enum class AudioWorkerSessionState
{
    AwaitingHello,
    Ready,
    Capturing,
    Closed,
    Faulted,
    Disconnected,
};

enum class AudioWorkerPollResult
{
    NoData,
    PcmEmitted,
    FaultEmitted,
    Terminal,
};

// Emit is called synchronously on the adapter's control thread. Implementations
// must serialize/write the message before returning, must not retain its
// reference, and must not reenter AudioWorkerSession. False means that the
// transport is disconnected. The adapter never queues outbound messages.
class IAudioWorkerMessageEmitter
{
public:
    virtual ~IAudioWorkerMessageEmitter() = default;
    virtual bool Emit(const audio_ipc::Message& message) noexcept = 0;
};

// A single-threaded worker-process session adapter. Call HandleControl,
// Poll, and OnTransportDisconnected serially on one worker-process control
// thread. Construction and Hello are inert; only a valid Start request opens
// the explicitly selected endpoint. Poll emits at most one bounded message.
class AudioWorkerSession final
{
public:
    explicit AudioWorkerSession(
        std::unique_ptr<ICaptureDeviceFactory> factory = {},
        std::size_t queueCapacityFrames = AudioCaptureWorker::c_defaultCapacityFrames,
        std::chrono::milliseconds startupTimeout = AudioCaptureWorker::c_defaultStartupTimeout);
    ~AudioWorkerSession();

    AudioWorkerSession(const AudioWorkerSession&) = delete;
    AudioWorkerSession& operator=(const AudioWorkerSession&) = delete;
    AudioWorkerSession(AudioWorkerSession&&) = delete;
    AudioWorkerSession& operator=(AudioWorkerSession&&) = delete;

    AudioWorkerSessionState State() const noexcept;

    // Valid control messages produce exactly one synchronous response. Invalid
    // messages produce a terminal Fault where an outbound sequence remains.
    AudioWorkerSessionState HandleControl(const audio_ipc::Message& request, IAudioWorkerMessageEmitter& emitter) noexcept;

    // Poll does not wait for new PCM and emits no more than one PCM or Fault
    // response, with PCM capped at 48,000 frames / 192,000 bytes. It can block
    // while the synchronous emitter writes or while failure cleanup joins the
    // capture worker; a future process supervisor must enforce a hard deadline.
    AudioWorkerPollResult Poll(IAudioWorkerMessageEmitter& emitter) noexcept;

    // Call after pipe/transport loss. It stops the worker and never reopens an
    // endpoint. Stop joins the existing capture thread and has no hard deadline;
    // a future host process supervisor must enforce a timeout by killing process.
    void OnTransportDisconnected() noexcept;

    // Shared sequence arithmetic for boundary validation. Sequence zero is not
    // used; a value after UINT64_MAX does not exist and never wraps to one.
    static bool IsImmediatelyAfter(std::uint64_t previous, std::uint64_t candidate) noexcept;

private:
    AudioWorkerSessionState Fail(AudioWorkerFaultCode code, IAudioWorkerMessageEmitter& emitter) noexcept;
    bool EmitMessage(audio_ipc::Message& message, IAudioWorkerMessageEmitter& emitter) noexcept;
    bool EmitControl(audio_ipc::MessageType type, std::uint64_t epoch, IAudioWorkerMessageEmitter& emitter) noexcept;
    bool EmitFault(AudioWorkerFaultCode code, std::uint64_t epoch, IAudioWorkerMessageEmitter& emitter) noexcept;
    bool EnsureOutputSequence(IAudioWorkerMessageEmitter& emitter) noexcept;
    bool StopCapture(AudioWorkerFaultCode& stopFault) noexcept;
    AudioWorkerFaultCode MapCaptureError(CaptureErrorCode code) const noexcept;

    AudioCaptureWorker m_worker;
    AudioWorkerSessionState m_state{AudioWorkerSessionState::AwaitingHello};
    std::uint64_t m_lastInboundSequence{};
    std::uint64_t m_lastOutboundSequence{};
    std::uint64_t m_highestEpoch{};
    std::uint64_t m_activeEpoch{};
    bool m_workerStarted{};
};

} // namespace wsl::virtio_sound::audio_worker
