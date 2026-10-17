// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "AudioWorkerSession.hpp"

namespace wsl::virtio_sound::audio_worker {
namespace {

    constexpr std::uint64_t c_maximumSequence = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::size_t c_maximumPcmFrames = 48000U;
    constexpr std::size_t c_bytesPerFrame = RequiredBytesPerFrame();

    bool IsTerminal(AudioWorkerSessionState state) noexcept
    {
        return state == AudioWorkerSessionState::Closed || state == AudioWorkerSessionState::Faulted ||
               state == AudioWorkerSessionState::Disconnected;
    }

    bool IsBlankEndpoint(const std::u16string& endpoint) noexcept
    {
        return endpoint.empty() || std::all_of(endpoint.begin(), endpoint.end(), [](char16_t character) {
                   return character == u' ' || character == u'\t' || character == u'\r' || character == u'\n';
               });
    }

    bool ConvertEndpointToWide(const std::u16string& endpoint, std::wstring& output)
    {
        output.clear();
        output.reserve(endpoint.size());
        if constexpr (sizeof(wchar_t) == sizeof(char16_t))
        {
            for (const char16_t codeUnit : endpoint)
            {
                output.push_back(static_cast<wchar_t>(codeUnit));
            }
        }
        else
        {
            for (std::size_t index = 0U; index < endpoint.size(); ++index)
            {
                std::uint32_t codePoint = static_cast<std::uint16_t>(endpoint[index]);
                if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
                {
                    if (index + 1U >= endpoint.size())
                    {
                        return false;
                    }
                    const std::uint32_t low = static_cast<std::uint16_t>(endpoint[index + 1U]);
                    if (low < 0xDC00U || low > 0xDFFFU)
                    {
                        return false;
                    }
                    codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
                    ++index;
                }
                else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
                {
                    return false;
                }
                output.push_back(static_cast<wchar_t>(codePoint));
            }
        }
        return true;
    }

    void WriteUint32LittleEndian(std::uint32_t value, std::uint8_t* destination) noexcept
    {
        destination[0] = static_cast<std::uint8_t>(value & 0xFFU);
        destination[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
        destination[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
        destination[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    }

    AudioWorkerFaultCode WorkerFaultFromSnapshot(const WorkerSnapshot& snapshot) noexcept
    {
        if (snapshot.m_lastError.m_code != CaptureErrorCode::None)
        {
            switch (snapshot.m_lastError.m_code)
            {
            case CaptureErrorCode::InvalidEndpointId:
                return AudioWorkerFaultCode::InvalidEndpoint;
            case CaptureErrorCode::BufferOverflow:
                return AudioWorkerFaultCode::CaptureQueueOverflow;
            case CaptureErrorCode::CaptureDiscontinuity:
                return AudioWorkerFaultCode::CaptureDiscontinuity;
            case CaptureErrorCode::InvalidPacket:
                return AudioWorkerFaultCode::InvalidCapturePacket;
            case CaptureErrorCode::StartupTimeout:
                return AudioWorkerFaultCode::CaptureStartupTimeout;
            case CaptureErrorCode::StopFailed:
                return AudioWorkerFaultCode::CaptureStopFailed;
            case CaptureErrorCode::None:
                break;
            case CaptureErrorCode::Busy:
            case CaptureErrorCode::FactoryUnavailable:
            case CaptureErrorCode::EndpointOpenFailed:
            case CaptureErrorCode::EndpointIsNotCapture:
            case CaptureErrorCode::UnsupportedFormat:
            case CaptureErrorCode::StartFailed:
                return AudioWorkerFaultCode::CaptureStartFailed;
            case CaptureErrorCode::ReadFailed:
            case CaptureErrorCode::WorkerFailed:
                return AudioWorkerFaultCode::CaptureReadFailed;
            }
        }
        if (snapshot.m_overflowEvents != 0U)
        {
            return AudioWorkerFaultCode::CaptureQueueOverflow;
        }
        if (snapshot.m_discontinuities != 0U)
        {
            return AudioWorkerFaultCode::CaptureDiscontinuity;
        }
        if (snapshot.m_invalidReads != 0U)
        {
            return AudioWorkerFaultCode::InvalidCapturePacket;
        }
        return AudioWorkerFaultCode::CaptureUnavailable;
    }

} // namespace

AudioWorkerSession::AudioWorkerSession(std::unique_ptr<ICaptureDeviceFactory> factory, std::size_t queueCapacityFrames, std::chrono::milliseconds startupTimeout) :
    m_worker(std::move(factory), queueCapacityFrames, startupTimeout)
{
}

AudioWorkerSession::~AudioWorkerSession()
{
    if (m_workerStarted)
    {
        AudioWorkerFaultCode ignored = AudioWorkerFaultCode::InternalFailure;
        (void)StopCapture(ignored);
    }
}

AudioWorkerSessionState AudioWorkerSession::State() const noexcept
{
    return m_state;
}

bool AudioWorkerSession::IsImmediatelyAfter(std::uint64_t previous, std::uint64_t candidate) noexcept
{
    return previous != c_maximumSequence && candidate == previous + 1U;
}

AudioWorkerSessionState AudioWorkerSession::HandleControl(const audio_ipc::Message& request, IAudioWorkerMessageEmitter& emitter) noexcept
{
    if (IsTerminal(m_state))
    {
        return m_state;
    }

    if (request.m_payload.size() > audio_ipc::c_maxPayloadBytes || request.m_header.m_payloadBytes != request.m_payload.size())
    {
        return Fail(AudioWorkerFaultCode::ProtocolViolation, emitter);
    }

    audio_ipc::Error validationError = audio_ipc::Error::None;
    if (!audio_ipc::ValidateMessage(request, validationError))
    {
        const AudioWorkerFaultCode code = IsImmediatelyAfter(m_lastInboundSequence, request.m_header.m_sequence)
                                              ? AudioWorkerFaultCode::ProtocolViolation
                                              : AudioWorkerFaultCode::SequenceViolation;
        return Fail(code, emitter);
    }

    if (!IsImmediatelyAfter(m_lastInboundSequence, request.m_header.m_sequence))
    {
        return Fail(AudioWorkerFaultCode::SequenceViolation, emitter);
    }
    m_lastInboundSequence = request.m_header.m_sequence;

    if (!EnsureOutputSequence(emitter))
    {
        return m_state;
    }

    try
    {
        switch (request.m_header.m_type)
        {
        case audio_ipc::MessageType::Hello:
            if (m_state != AudioWorkerSessionState::AwaitingHello)
            {
                return Fail(AudioWorkerFaultCode::InvalidSessionState, emitter);
            }
            if (!EmitControl(audio_ipc::MessageType::Ready, 0U, emitter))
            {
                return m_state;
            }
            m_state = AudioWorkerSessionState::Ready;
            return m_state;

        case audio_ipc::MessageType::Start:
        {
            if (m_state != AudioWorkerSessionState::Ready || request.m_header.m_epoch <= m_highestEpoch)
            {
                return Fail(AudioWorkerFaultCode::InvalidSessionState, emitter);
            }
            m_highestEpoch = request.m_header.m_epoch;

            std::u16string endpoint;
            if (!audio_ipc::ParseStartPayload(request.m_payload.data(), request.m_payload.size(), endpoint, validationError) ||
                IsBlankEndpoint(endpoint))
            {
                return Fail(AudioWorkerFaultCode::InvalidEndpoint, emitter);
            }

            std::wstring wideEndpoint;
            if (!ConvertEndpointToWide(endpoint, wideEndpoint))
            {
                return Fail(AudioWorkerFaultCode::InvalidEndpoint, emitter);
            }

            CaptureError captureError;
            if (!m_worker.Start(wideEndpoint, captureError))
            {
                return Fail(MapCaptureError(captureError.m_code), emitter);
            }
            m_workerStarted = true;
            if (!EmitControl(audio_ipc::MessageType::Started, request.m_header.m_epoch, emitter))
            {
                return m_state;
            }
            m_activeEpoch = request.m_header.m_epoch;
            m_state = AudioWorkerSessionState::Capturing;
            return m_state;
        }

        case audio_ipc::MessageType::Stop:
            if (m_state != AudioWorkerSessionState::Capturing || request.m_header.m_epoch != m_activeEpoch)
            {
                return Fail(AudioWorkerFaultCode::InvalidSessionState, emitter);
            }
            {
                AudioWorkerFaultCode stopFault = AudioWorkerFaultCode::CaptureStopFailed;
                if (!StopCapture(stopFault))
                {
                    return Fail(stopFault, emitter);
                }
            }
            if (!EmitControl(audio_ipc::MessageType::Stopped, request.m_header.m_epoch, emitter))
            {
                return m_state;
            }
            m_state = AudioWorkerSessionState::Ready;
            return m_state;

        case audio_ipc::MessageType::Shutdown:
            if (m_state != AudioWorkerSessionState::Ready && m_state != AudioWorkerSessionState::Capturing)
            {
                return Fail(AudioWorkerFaultCode::InvalidSessionState, emitter);
            }
            {
                AudioWorkerFaultCode stopFault = AudioWorkerFaultCode::CaptureStopFailed;
                if (!StopCapture(stopFault))
                {
                    return Fail(stopFault, emitter);
                }
            }
            if (!EmitControl(audio_ipc::MessageType::Closed, 0U, emitter))
            {
                return m_state;
            }
            m_state = AudioWorkerSessionState::Closed;
            return m_state;

        case audio_ipc::MessageType::Ready:
        case audio_ipc::MessageType::Started:
        case audio_ipc::MessageType::Stopped:
        case audio_ipc::MessageType::Pcm:
        case audio_ipc::MessageType::Fault:
        case audio_ipc::MessageType::Closed:
        case audio_ipc::MessageType::ServerIdentity:
            return Fail(AudioWorkerFaultCode::InvalidSessionState, emitter);
        }
    }
    catch (...)
    {
        return Fail(AudioWorkerFaultCode::InternalFailure, emitter);
    }

    return Fail(AudioWorkerFaultCode::ProtocolViolation, emitter);
}

AudioWorkerPollResult AudioWorkerSession::Poll(IAudioWorkerMessageEmitter& emitter) noexcept
{
    if (IsTerminal(m_state))
    {
        return AudioWorkerPollResult::Terminal;
    }
    if (m_state != AudioWorkerSessionState::Capturing || !m_workerStarted)
    {
        return AudioWorkerPollResult::NoData;
    }
    const std::uint64_t previousOutboundSequence = m_lastOutboundSequence;
    if (!EnsureOutputSequence(emitter))
    {
        return m_lastOutboundSequence > previousOutboundSequence ? AudioWorkerPollResult::FaultEmitted : AudioWorkerPollResult::Terminal;
    }

    try
    {
        WorkerSnapshot snapshot = m_worker.Snapshot();
        if (snapshot.m_state != WorkerState::Running || snapshot.m_overflowEvents != 0U || snapshot.m_discontinuities != 0U ||
            snapshot.m_invalidReads != 0U)
        {
            (void)Fail(WorkerFaultFromSnapshot(snapshot), emitter);
            return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
        }
        if (snapshot.m_queuedFrames == 0U)
        {
            return AudioWorkerPollResult::NoData;
        }

        const std::size_t requestedFrames = (std::min)(snapshot.m_queuedFrames, c_maximumPcmFrames);
        if (requestedFrames == 0U || requestedFrames > c_maximumPcmFrames || requestedFrames > audio_ipc::c_maxPayloadBytes / c_bytesPerFrame)
        {
            (void)Fail(AudioWorkerFaultCode::InvalidCapturePacket, emitter);
            return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
        }
        const std::size_t requestedBytes = requestedFrames * c_bytesPerFrame;

        audio_ipc::Message response;
        response.m_header.m_type = audio_ipc::MessageType::Pcm;
        response.m_header.m_epoch = m_activeEpoch;
        response.m_payload.resize(requestedBytes);
        const std::size_t framesRead = m_worker.ReadFrames(response.m_payload.data(), requestedFrames, requestedBytes);
        if (framesRead > requestedFrames)
        {
            (void)Fail(AudioWorkerFaultCode::InvalidCapturePacket, emitter);
            return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
        }
        if (framesRead == 0U)
        {
            snapshot = m_worker.Snapshot();
            if (snapshot.m_state != WorkerState::Running || snapshot.m_overflowEvents != 0U || snapshot.m_discontinuities != 0U ||
                snapshot.m_invalidReads != 0U)
            {
                (void)Fail(WorkerFaultFromSnapshot(snapshot), emitter);
                return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
            }
            return AudioWorkerPollResult::NoData;
        }

        response.m_payload.resize(framesRead * c_bytesPerFrame);
        snapshot = m_worker.Snapshot();
        if (snapshot.m_state != WorkerState::Running || snapshot.m_overflowEvents != 0U || snapshot.m_discontinuities != 0U ||
            snapshot.m_invalidReads != 0U)
        {
            (void)Fail(WorkerFaultFromSnapshot(snapshot), emitter);
            return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
        }

        if (!EmitMessage(response, emitter))
        {
            return AudioWorkerPollResult::Terminal;
        }
        return AudioWorkerPollResult::PcmEmitted;
    }
    catch (...)
    {
        (void)Fail(AudioWorkerFaultCode::InternalFailure, emitter);
        return m_state == AudioWorkerSessionState::Disconnected ? AudioWorkerPollResult::Terminal : AudioWorkerPollResult::FaultEmitted;
    }
}

void AudioWorkerSession::OnTransportDisconnected() noexcept
{
    if (m_state == AudioWorkerSessionState::Disconnected)
    {
        return;
    }
    AudioWorkerFaultCode ignored = AudioWorkerFaultCode::CaptureStopFailed;
    try
    {
        (void)StopCapture(ignored);
    }
    catch (...)
    {
        m_workerStarted = false;
        m_activeEpoch = 0U;
    }
    m_state = AudioWorkerSessionState::Disconnected;
}

AudioWorkerSessionState AudioWorkerSession::Fail(AudioWorkerFaultCode code, IAudioWorkerMessageEmitter& emitter) noexcept
{
    if (IsTerminal(m_state))
    {
        return m_state;
    }

    m_state = AudioWorkerSessionState::Faulted;
    const std::uint64_t faultEpoch = m_activeEpoch != 0U ? m_activeEpoch : m_highestEpoch;
    AudioWorkerFaultCode stopFault = AudioWorkerFaultCode::CaptureStopFailed;
    try
    {
        if (!StopCapture(stopFault) && stopFault == AudioWorkerFaultCode::CaptureStopFailed)
        {
            code = stopFault;
        }
    }
    catch (...)
    {
        m_workerStarted = false;
        m_activeEpoch = 0U;
        code = AudioWorkerFaultCode::InternalFailure;
    }
    m_activeEpoch = 0U;
    (void)EmitFault(code, faultEpoch, emitter);
    return m_state;
}

bool AudioWorkerSession::EmitMessage(audio_ipc::Message& message, IAudioWorkerMessageEmitter& emitter) noexcept
{
    if (m_lastOutboundSequence == c_maximumSequence || message.m_payload.size() > audio_ipc::c_maxPayloadBytes)
    {
        return false;
    }

    message.m_header.m_magic = audio_ipc::c_magic;
    message.m_header.m_version = audio_ipc::c_version;
    message.m_header.m_payloadBytes = static_cast<std::uint32_t>(message.m_payload.size());
    message.m_header.m_reserved = 0U;
    message.m_header.m_sequence = m_lastOutboundSequence + 1U;

    audio_ipc::Error validationError = audio_ipc::Error::None;
    if (!audio_ipc::ValidateMessage(message, validationError))
    {
        AudioWorkerFaultCode ignored = AudioWorkerFaultCode::InternalFailure;
        try
        {
            (void)StopCapture(ignored);
        }
        catch (...)
        {
            m_workerStarted = false;
            m_activeEpoch = 0U;
        }
        m_state = AudioWorkerSessionState::Faulted;
        return false;
    }

    if (!emitter.Emit(message))
    {
        OnTransportDisconnected();
        return false;
    }
    m_lastOutboundSequence = message.m_header.m_sequence;
    return true;
}

bool AudioWorkerSession::EmitControl(audio_ipc::MessageType type, std::uint64_t epoch, IAudioWorkerMessageEmitter& emitter) noexcept
{
    audio_ipc::Message message;
    message.m_header.m_type = type;
    message.m_header.m_epoch = epoch;
    return EmitMessage(message, emitter);
}

bool AudioWorkerSession::EmitFault(AudioWorkerFaultCode code, std::uint64_t epoch, IAudioWorkerMessageEmitter& emitter) noexcept
{
    try
    {
        audio_ipc::Message message;
        message.m_header.m_type = audio_ipc::MessageType::Fault;
        message.m_header.m_epoch = epoch;
        message.m_payload.resize(sizeof(std::uint32_t));
        WriteUint32LittleEndian(static_cast<std::uint32_t>(code), message.m_payload.data());
        return EmitMessage(message, emitter);
    }
    catch (...)
    {
        m_state = AudioWorkerSessionState::Faulted;
        return false;
    }
}

bool AudioWorkerSession::EnsureOutputSequence(IAudioWorkerMessageEmitter& emitter) noexcept
{
    if (m_lastOutboundSequence < c_maximumSequence - 1U)
    {
        return true;
    }
    (void)Fail(AudioWorkerFaultCode::OutboundSequenceExhausted, emitter);
    return false;
}

bool AudioWorkerSession::StopCapture(AudioWorkerFaultCode& stopFault) noexcept
{
    CaptureError error;
    bool stopped = false;
    try
    {
        stopped = m_worker.Stop(error);
    }
    catch (...)
    {
        m_workerStarted = false;
        m_activeEpoch = 0U;
        stopFault = AudioWorkerFaultCode::CaptureStopFailed;
        return false;
    }
    m_workerStarted = false;
    m_activeEpoch = 0U;
    if (!stopped)
    {
        stopFault = MapCaptureError(error.m_code);
        return false;
    }
    return true;
}

AudioWorkerFaultCode AudioWorkerSession::MapCaptureError(CaptureErrorCode code) const noexcept
{
    switch (code)
    {
    case CaptureErrorCode::None:
        return AudioWorkerFaultCode::CaptureUnavailable;
    case CaptureErrorCode::InvalidEndpointId:
        return AudioWorkerFaultCode::InvalidEndpoint;
    case CaptureErrorCode::Busy:
    case CaptureErrorCode::FactoryUnavailable:
    case CaptureErrorCode::EndpointOpenFailed:
    case CaptureErrorCode::EndpointIsNotCapture:
    case CaptureErrorCode::UnsupportedFormat:
    case CaptureErrorCode::StartFailed:
        return AudioWorkerFaultCode::CaptureStartFailed;
    case CaptureErrorCode::ReadFailed:
    case CaptureErrorCode::WorkerFailed:
        return AudioWorkerFaultCode::CaptureReadFailed;
    case CaptureErrorCode::InvalidPacket:
        return AudioWorkerFaultCode::InvalidCapturePacket;
    case CaptureErrorCode::BufferOverflow:
        return AudioWorkerFaultCode::CaptureQueueOverflow;
    case CaptureErrorCode::CaptureDiscontinuity:
        return AudioWorkerFaultCode::CaptureDiscontinuity;
    case CaptureErrorCode::StartupTimeout:
        return AudioWorkerFaultCode::CaptureStartupTimeout;
    case CaptureErrorCode::StopFailed:
        return AudioWorkerFaultCode::CaptureStopFailed;
    }
    return AudioWorkerFaultCode::CaptureUnavailable;
}

} // namespace wsl::virtio_sound::audio_worker
