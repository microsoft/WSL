// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "AudioWorkerSession.hpp"

#if defined(_WIN32)
#include "../src/windows/ServerIdentityValidation.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace wsl::virtio_sound::audio_ipc;
using namespace wsl::virtio_sound::audio_worker;

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct FakeCaptureControl
{
    std::atomic<int> createCalls{};
    std::atomic<int> openCalls{};
    std::atomic<int> startCalls{};
    std::atomic<int> closeCalls{};
    std::atomic<int> readCalls{};
    std::atomic<int> packetReturns{};
    std::atomic<int> lastPacketReadCall{};
    std::atomic<bool> failOpen{};
    std::atomic<bool> failRead{};
    std::atomic<bool> failClose{};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<CapturePacket> packets;
    std::wstring openedEndpoint;
};

class FakeCaptureDevice final : public ICaptureDevice
{
public:
    explicit FakeCaptureDevice(std::shared_ptr<FakeCaptureControl> control) : control_(std::move(control))
    {
    }

    bool OpenEndpoint(const std::wstring& endpointId, PcmFormat& negotiatedFormat, CaptureError& error) override
    {
        ++control_->openCalls;
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            control_->openedEndpoint = endpointId;
        }
        if (control_->failOpen.load())
        {
            error = {CaptureErrorCode::EndpointOpenFailed, 40L, L"fake open failure"};
            return false;
        }
        negotiatedFormat = RequiredCaptureFormat();
        error = {};
        return true;
    }

    bool Start(CaptureError& error) override
    {
        ++control_->startCalls;
        error = {};
        return true;
    }

    CaptureReadResult ReadPacket(std::chrono::milliseconds timeout, const std::atomic<bool>& stopRequested, CapturePacket& packet, CaptureError& error) override
    {
        ++control_->readCalls;
        std::unique_lock<std::mutex> lock(control_->mutex);
        const auto shortTimeout = (std::min)(timeout, 5ms);
        control_->condition.wait_for(
            lock, shortTimeout, [&] { return !control_->packets.empty() || control_->failRead.load() || stopRequested.load(); });
        if (stopRequested.load())
        {
            return CaptureReadResult::Stopped;
        }
        if (control_->failRead.load())
        {
            error = {CaptureErrorCode::ReadFailed, 41L, L"fake read failure"};
            return CaptureReadResult::Failed;
        }
        if (control_->packets.empty())
        {
            return CaptureReadResult::Timeout;
        }
        packet = std::move(control_->packets.front());
        control_->packets.pop_front();
        control_->lastPacketReadCall.store(control_->readCalls.load());
        ++control_->packetReturns;
        return CaptureReadResult::Packet;
    }

    bool Close(CaptureError& error) override
    {
        ++control_->closeCalls;
        if (control_->failClose.load())
        {
            error = {CaptureErrorCode::StopFailed, 42L, L"fake close failure"};
            return false;
        }
        error = {};
        return true;
    }

private:
    std::shared_ptr<FakeCaptureControl> control_;
};

class FakeCaptureDeviceFactory final : public ICaptureDeviceFactory
{
public:
    explicit FakeCaptureDeviceFactory(std::shared_ptr<FakeCaptureControl> control) : control_(std::move(control))
    {
    }

    std::unique_ptr<ICaptureDevice> Create() override
    {
        ++control_->createCalls;
        return std::make_unique<FakeCaptureDevice>(control_);
    }

private:
    std::shared_ptr<FakeCaptureControl> control_;
};

class CollectingEmitter final : public IAudioWorkerMessageEmitter
{
public:
    bool Emit(const Message& message) noexcept override
    {
        if (failType.has_value() && message.m_header.m_type == *failType)
        {
            return false;
        }
        try
        {
            messages.push_back(message);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<Message> messages;
    std::optional<MessageType> failType;
};

Message MakeControl(MessageType type, std::uint64_t sequence, std::uint64_t epoch = 0U, std::vector<std::uint8_t> payload = {})
{
    Message message;
    message.m_header.m_type = type;
    message.m_header.m_epoch = epoch;
    message.m_header.m_sequence = sequence;
    message.m_header.m_payloadBytes = static_cast<std::uint32_t>(payload.size());
    message.m_payload = std::move(payload);
    return message;
}

Message MakeStart(std::uint64_t sequence, std::uint64_t epoch)
{
    std::vector<std::uint8_t> payload;
    Error error = Error::None;
    Require(BuildStartPayload(u"fake-capture-endpoint", payload, error), "test Start payload should encode");
    return MakeControl(MessageType::Start, sequence, epoch, std::move(payload));
}

std::unique_ptr<ICaptureDeviceFactory> MakeFactory(const std::shared_ptr<FakeCaptureControl>& control)
{
    return std::make_unique<FakeCaptureDeviceFactory>(control);
}

void PushPacket(const std::shared_ptr<FakeCaptureControl>& control, std::uint32_t frames, std::uint8_t firstByte, bool discontinuity = false)
{
    CapturePacket packet;
    packet.m_frames = frames;
    packet.m_discontinuity = discontinuity;
    packet.m_s16LeBytes.resize(static_cast<std::size_t>(frames) * RequiredBytesPerFrame());
    for (std::size_t index = 0U; index < packet.m_s16LeBytes.size(); ++index)
    {
        packet.m_s16LeBytes[index] = static_cast<std::uint8_t>(firstByte + index);
    }
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        control->packets.push_back(std::move(packet));
    }
    control->condition.notify_all();
}

void TriggerReadFailure(const std::shared_ptr<FakeCaptureControl>& control)
{
    control->failRead.store(true);
    control->condition.notify_all();
}

bool WaitForConsumedPackets(const std::shared_ptr<FakeCaptureControl>& control, int expectedPackets, std::chrono::milliseconds timeout = 1500ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (control->packetReturns.load() >= expectedPackets)
        {
            const int packetReadCall = control->lastPacketReadCall.load();
            if (control->readCalls.load() > packetReadCall)
            {
                return true;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool WaitForPoll(AudioWorkerSession& session, CollectingEmitter& emitter, AudioWorkerPollResult expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const AudioWorkerPollResult result = session.Poll(emitter);
        if (result == expected)
        {
            return true;
        }
        if (result == AudioWorkerPollResult::Terminal || result == AudioWorkerPollResult::FaultEmitted)
        {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

std::uint32_t FaultCode(const Message& message)
{
    Require(message.m_header.m_type == MessageType::Fault && message.m_payload.size() == 4U, "Fault must carry a 4-byte code");
    return static_cast<std::uint32_t>(message.m_payload[0]) | (static_cast<std::uint32_t>(message.m_payload[1]) << 8U) |
           (static_cast<std::uint32_t>(message.m_payload[2]) << 16U) | (static_cast<std::uint32_t>(message.m_payload[3]) << 24U);
}

void TestConstructionAndHelloAreInert()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    Require(
        control->createCalls.load() == 0 && control->openCalls.load() == 0,
        "session construction must not create or open a capture endpoint");

    Require(
        session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter) == AudioWorkerSessionState::Ready,
        "Hello should transition to Ready");
    Require(
        emitter.messages.size() == 1U && emitter.messages[0].m_header.m_type == MessageType::Ready &&
            emitter.messages[0].m_header.m_sequence == 1U,
        "Hello should synchronously emit Ready as outbound sequence one");
    Require(
        control->createCalls.load() == 0 && control->openCalls.load() == 0 && control->startCalls.load() == 0,
        "Hello must not touch the capture factory or endpoint");
}

void TestLifecyclePcmAndShutdownOrdering()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    Require(
        session.State() == AudioWorkerSessionState::Capturing, "Start should acknowledge only after the fake worker has started");
    Require(
        emitter.messages.size() == 2U && emitter.messages[1].m_header.m_type == MessageType::Started &&
            emitter.messages[1].m_header.m_epoch == 1U && emitter.messages[1].m_header.m_sequence == 2U,
        "Started must be emitted synchronously before any Poll PCM");
    Require(
        control->openCalls.load() == 1 && control->startCalls.load() == 1,
        "valid Start should open the exact requested fake endpoint once");

    PushPacket(control, 2U, 31U);
    Require(
        WaitForPoll(session, emitter, AudioWorkerPollResult::PcmEmitted), "Poll should emit the queued fake PCM after Started");
    const Message& pcm = emitter.messages.back();
    Require(
        pcm.m_header.m_type == MessageType::Pcm && pcm.m_header.m_epoch == 1U && pcm.m_header.m_sequence == 3U &&
            pcm.m_payload.size() == 8U && pcm.m_payload[0] == 31U,
        "PCM should carry the current epoch, next output sequence, and exact bytes");

    (void)session.HandleControl(MakeControl(MessageType::Stop, 3U, 1U), emitter);
    Require(
        session.State() == AudioWorkerSessionState::Ready && emitter.messages.back().m_header.m_type == MessageType::Stopped &&
            emitter.messages.back().m_header.m_sequence == 4U,
        "Stopped should be emitted only after worker Stop succeeds");
    Require(control->closeCalls.load() == 1, "Stop should close the selected endpoint once");

    (void)session.HandleControl(MakeControl(MessageType::Shutdown, 4U), emitter);
    Require(
        session.State() == AudioWorkerSessionState::Closed && emitter.messages.back().m_header.m_type == MessageType::Closed &&
            emitter.messages.back().m_header.m_sequence == 5U,
        "Shutdown should emit Closed after the worker is stopped");
    Require(session.Poll(emitter) == AudioWorkerPollResult::Terminal, "closed session must not emit later PCM");
}

void TestRestartNeedsNewEpochAndDropsOldQueuedPcm()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 64U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    PushPacket(control, 4U, 90U);
    Require(WaitForConsumedPackets(control, 1), "fake worker should consume the old-epoch packet before Stop");

    (void)session.HandleControl(MakeControl(MessageType::Stop, 3U, 1U), emitter);
    (void)session.HandleControl(MakeStart(4U, 2U), emitter);
    Require(
        session.State() == AudioWorkerSessionState::Capturing && control->startCalls.load() == 2,
        "a strictly higher epoch should restart capture successfully");
    const std::size_t beforePoll = emitter.messages.size();
    Require(
        session.Poll(emitter) == AudioWorkerPollResult::NoData && emitter.messages.size() == beforePoll,
        "PCM queued in the stopped epoch must be discarded before the restart");

    (void)session.HandleControl(MakeControl(MessageType::Shutdown, 5U), emitter);
    Require(session.State() == AudioWorkerSessionState::Closed, "restart test should close its worker cleanly");
}

void TestWrongEpochStopFaultsAndStopsCapture()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 5U), emitter);
    (void)session.HandleControl(MakeControl(MessageType::Stop, 3U, 4U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            control->closeCalls.load() == 1,
        "wrong-epoch Stop must fault and stop the active worker");
    Require(
        std::none_of(
            emitter.messages.begin(),
            emitter.messages.end(),
            [](const Message& message) { return message.m_header.m_type == MessageType::Stopped; }),
        "wrong-epoch Stop must never receive a Stopped acknowledgement");
}

void TestPcmPollCapsMessagesAt48000Frames()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 60000U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    PushPacket(control, 30000U, 1U);
    PushPacket(control, 30000U, 2U);
    Require(WaitForConsumedPackets(control, 2), "fake worker should consume both packets larger than one outbound frame");

    Require(session.Poll(emitter) == AudioWorkerPollResult::PcmEmitted, "Poll should emit the first bounded PCM message");
    const Message& firstPcm = emitter.messages.back();
    Require(
        firstPcm.m_header.m_type == MessageType::Pcm && firstPcm.m_payload.size() == 192000U,
        "one outbound PCM message must not exceed 48,000 frames or 192,000 bytes");
    Require(
        session.Poll(emitter) == AudioWorkerPollResult::PcmEmitted && emitter.messages.back().m_payload.size() == 48000U,
        "remaining frames should be delivered in a separate bounded message");

    (void)session.HandleControl(MakeControl(MessageType::Shutdown, 3U), emitter);
    Require(session.State() == AudioWorkerSessionState::Closed, "PCM cap test should stop before closing");
}

void TestStaleEpochIsTerminalAndDoesNotReopen()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 7U), emitter);
    (void)session.HandleControl(MakeControl(MessageType::Stop, 3U, 7U), emitter);
    (void)session.HandleControl(MakeStart(4U, 7U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::InvalidSessionState),
        "reusing a stopped epoch must produce a terminal fixed-code Fault");
    Require(
        control->openCalls.load() == 1 && control->startCalls.load() == 1,
        "stale epoch rejection must not reopen or restart capture");
}

void TestOutOfOrderSequenceFaultsBeforeStart()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(3U, 1U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::SequenceViolation),
        "an out-of-order input sequence must terminate with SequenceViolation");
    Require(
        control->createCalls.load() == 0 && control->openCalls.load() == 0,
        "sequence rejection must happen before any capture factory activity");
}

void TestOversizedDecodedMessageIsRejectedBeforeParsingOrStart()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);

    Message oversized = MakeControl(MessageType::Start, 2U, 1U);
    oversized.m_payload.resize(c_maxPayloadBytes + 1U, 0U);
    oversized.m_header.m_payloadBytes = static_cast<std::uint32_t>(oversized.m_payload.size());
    (void)session.HandleControl(oversized, emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::ProtocolViolation),
        "oversized already-decoded payload must be rejected as protocol failure");
    Require(
        control->createCalls.load() == 0 && control->openCalls.load() == 0,
        "oversized Start must be rejected before parse, allocation, or device creation");
}

void TestCaptureStartFailureMapsToFixedCodeAndPreservesEpoch()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->failOpen.store(true);
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 9U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            emitter.messages.back().m_header.m_epoch == 9U &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::CaptureStartFailed),
        "capture activation failure should emit a fixed code with the candidate epoch");
    Require(
        std::none_of(
            emitter.messages.begin(),
            emitter.messages.end(),
            [](const Message& message) { return message.m_header.m_type == MessageType::Started; }),
        "capture activation failure must not emit Started");
}

void TestReadFailureDoesNotEmitQueuedOrFallbackPcm()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    TriggerReadFailure(control);

    Require(WaitForPoll(session, emitter, AudioWorkerPollResult::FaultEmitted), "read failure should emit a terminal Fault");
    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::CaptureReadFailed),
        "read failure should map to the explicit CaptureReadFailed code");
    Require(
        std::none_of(
            emitter.messages.begin(),
            emitter.messages.end(),
            [](const Message& message) { return message.m_header.m_type == MessageType::Pcm; }),
        "failed capture must not expose stale, queued, or synthetic PCM");
}

void TestStopFailureNeverAcknowledgesStopped()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->failClose.store(true);
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    (void)session.HandleControl(MakeControl(MessageType::Stop, 3U, 1U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault &&
            emitter.messages.back().m_header.m_epoch == 1U &&
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::CaptureStopFailed),
        "worker close failure must fault without acknowledging Stopped");
    Require(
        std::none_of(
            emitter.messages.begin(),
            emitter.messages.end(),
            [](const Message& message) { return message.m_header.m_type == MessageType::Stopped; }),
        "no Stopped response may follow a failed worker Stop");
}

void TestOverflowAndDiscontinuityFaultsAreExplicit()
{
    {
        auto control = std::make_shared<FakeCaptureControl>();
        AudioWorkerSession session(MakeFactory(control), 4U);
        CollectingEmitter emitter;
        (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
        (void)session.HandleControl(MakeStart(2U, 1U), emitter);
        PushPacket(control, 3U, 3U);
        PushPacket(control, 3U, 9U);
        const auto packetDeadline = std::chrono::steady_clock::now() + 1000ms;
        while (control->packetReturns.load() < 2 && std::chrono::steady_clock::now() < packetDeadline)
        {
            std::this_thread::sleep_for(2ms);
        }
        Require(WaitForPoll(session, emitter, AudioWorkerPollResult::FaultEmitted), "queue overflow should emit a terminal Fault");
        Require(
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::CaptureQueueOverflow),
            "overflow must use its explicit fixed fault code");
    }
    {
        auto control = std::make_shared<FakeCaptureControl>();
        AudioWorkerSession session(MakeFactory(control), 32U);
        CollectingEmitter emitter;
        (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
        (void)session.HandleControl(MakeStart(2U, 1U), emitter);
        PushPacket(control, 2U, 20U, true);
        const auto packetDeadline = std::chrono::steady_clock::now() + 1000ms;
        while (control->packetReturns.load() == 0 && std::chrono::steady_clock::now() < packetDeadline)
        {
            std::this_thread::sleep_for(2ms);
        }
        Require(WaitForPoll(session, emitter, AudioWorkerPollResult::FaultEmitted), "discontinuity should emit a terminal Fault");
        Require(
            FaultCode(emitter.messages.back()) == static_cast<std::uint32_t>(AudioWorkerFaultCode::CaptureDiscontinuity),
            "discontinuity must use its explicit fixed fault code");
    }
}

void TestShutdownCaptureFailureNeverEmitsClosed()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->failClose.store(true);
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    (void)session.HandleControl(MakeControl(MessageType::Shutdown, 3U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Faulted && emitter.messages.back().m_header.m_type == MessageType::Fault,
        "Shutdown must fault when capture cannot stop cleanly");
    Require(
        std::none_of(
            emitter.messages.begin(),
            emitter.messages.end(),
            [](const Message& message) { return message.m_header.m_type == MessageType::Closed; }),
        "Closed must not be emitted after a failed worker Stop");
}

void TestFailedStartedEmissionDisconnectsAndStops()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    emitter.failType = MessageType::Started;
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);

    Require(
        session.State() == AudioWorkerSessionState::Disconnected && control->closeCalls.load() == 1,
        "failed Started transport write must stop the worker and disconnect");
    Require(
        emitter.messages.size() == 1U && emitter.messages.back().m_header.m_type == MessageType::Ready &&
            session.Poll(emitter) == AudioWorkerPollResult::Terminal,
        "PCM must remain unavailable when Started was not emitted successfully");
}

void TestDisconnectStopsWithoutReopening()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession session(MakeFactory(control), 32U);
    CollectingEmitter emitter;
    (void)session.HandleControl(MakeControl(MessageType::Hello, 1U), emitter);
    (void)session.HandleControl(MakeStart(2U, 1U), emitter);
    session.OnTransportDisconnected();

    Require(
        session.State() == AudioWorkerSessionState::Disconnected && control->closeCalls.load() == 1,
        "transport disconnect must stop and close the active worker");
    const std::size_t emittedBefore = emitter.messages.size();
    (void)session.HandleControl(MakeStart(3U, 2U), emitter);
    Require(
        session.Poll(emitter) == AudioWorkerPollResult::Terminal && emitter.messages.size() == emittedBefore &&
            control->openCalls.load() == 1 && control->startCalls.load() == 1,
        "disconnected session must stay terminal and never reopen capture");
}

void TestSequenceArithmeticNeverWraps()
{
    constexpr std::uint64_t c_maximum = (std::numeric_limits<std::uint64_t>::max)();
    Require(AudioWorkerSession::IsImmediatelyAfter(0U, 1U), "the first sequence should be one");
    Require(AudioWorkerSession::IsImmediatelyAfter(c_maximum - 1U, c_maximum), "maximum sequence may be reached contiguously");
    Require(!AudioWorkerSession::IsImmediatelyAfter(c_maximum, 1U), "sequence validation must not wrap from maximum to one");
    Require(!AudioWorkerSession::IsImmediatelyAfter(8U, 10U), "sequence validation must reject gaps");
}

#if defined(_WIN32)
class TestHandle final
{
public:
    explicit TestHandle(HANDLE value = nullptr) noexcept : value_(value)
    {
    }
    ~TestHandle()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(value_);
        }
    }

    TestHandle(const TestHandle&) = delete;
    TestHandle& operator=(const TestHandle&) = delete;
    HANDLE Get() const noexcept
    {
        return value_;
    }

private:
    HANDLE value_{};
};

class LocalPipePair final
{
public:
    LocalPipePair() = default;
    ~LocalPipePair()
    {
        if (client_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(client_);
        }
        if (server_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(server_);
        }
    }

    LocalPipePair(const LocalPipePair&) = delete;
    LocalPipePair& operator=(const LocalPipePair&) = delete;
    LocalPipePair(LocalPipePair&& other) noexcept : server_(other.server_), client_(other.client_)
    {
        other.server_ = INVALID_HANDLE_VALUE;
        other.client_ = INVALID_HANDLE_VALUE;
    }

    HANDLE Client() const noexcept
    {
        return client_;
    }

    static LocalPipePair Create()
    {
        LocalPipePair pair;
        const std::wstring path = L"\\\\.\\pipe\\wsl-audio-identity-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64());
        pair.server_ = CreateNamedPipeW(
            path.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1U,
            4096U,
            4096U,
            0U,
            nullptr);
        if (pair.server_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("could not create fake identity pipe");
        }
        pair.client_ = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pair.client_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("could not connect fake identity pipe");
        }
        const BOOL connected = ConnectNamedPipe(pair.server_, nullptr);
        if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            throw std::runtime_error("could not accept fake identity pipe");
        }
        return pair;
    }

private:
    HANDLE server_{INVALID_HANDLE_VALUE};
    HANDLE client_{INVALID_HANDLE_VALUE};
};

std::wstring CurrentTestImagePath()
{
    std::vector<wchar_t> path(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        throw std::runtime_error("could not query fake host image path");
    }
    return std::wstring(path.data(), length);
}

void TestServerIdentityBootstrapParsingAndReplay()
{
    std::vector<std::uint8_t> payload;
    Error error = Error::None;
    Require(BuildServerIdentityPayload(1U, payload, error), "test handle value should encode for semantic validation");
    const Message bootstrap = MakeControl(MessageType::ServerIdentity, 1U, 0U, std::move(payload));

    std::uint64_t parsedHandle = 77U;
    Require(
        wsl::virtio_sound::audio_worker::windows::detail::ParseServerIdentityBootstrap(bootstrap, parsedHandle) && parsedHandle == 1U,
        "the one-time ServerIdentity bootstrap should parse only at its fixed sequence");
    Message wrongType = MakeControl(MessageType::Hello, 1U);
    parsedHandle = 77U;
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ParseServerIdentityBootstrap(wrongType, parsedHandle) && parsedHandle == 77U,
        "a missing or wrong-type bootstrap must fail without changing output");
    Message replay = bootstrap;
    replay.m_header.m_sequence = 2U;
    parsedHandle = 77U;
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ParseServerIdentityBootstrap(replay, parsedHandle) && parsedHandle == 77U,
        "a second bootstrap must not be accepted as the one-time sequence-1 message");

    const std::uint64_t pseudoHandles[] = {
        UINT64_C(0xffffffffffffffff),
        UINT64_C(0xfffffffffffffffe),
        UINT64_C(0xfffffffffffffffc),
        UINT64_C(0xfffffffffffffffb),
        UINT64_C(0xfffffffffffffffa),
        UINT64_C(0x00000000ffffffff),
        UINT64_C(0x00000000fffffffe),
        UINT64_C(0x00000000fffffffc),
        UINT64_C(0x00000000fffffffb),
        UINT64_C(0x00000000fffffffa),
    };
    for (const std::uint64_t value : pseudoHandles)
    {
        Require(
            !wsl::virtio_sound::audio_worker::windows::detail::IsRealProcessHandleValue(value),
            "native-width and zero-extended pseudo handles must be rejected");
    }
    if constexpr (sizeof(std::uintptr_t) == 4U)
    {
        Require(
            !wsl::virtio_sound::audio_worker::windows::detail::IsRealProcessHandleValue(UINT64_C(0x100000000)),
            "a handle value wider than the worker pointer must be rejected");
    }

    auto firstRequestControl = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession firstRequest(MakeFactory(firstRequestControl), 32U);
    CollectingEmitter firstEmitter;
    (void)firstRequest.HandleControl(bootstrap, firstEmitter);
    Require(
        firstRequest.State() == AudioWorkerSessionState::Faulted && firstRequestControl->openCalls.load() == 0,
        "ServerIdentity as a session request must fault before capture can open");

    auto replayControl = std::make_shared<FakeCaptureControl>();
    AudioWorkerSession afterHello(MakeFactory(replayControl), 32U);
    CollectingEmitter replayEmitter;
    (void)afterHello.HandleControl(MakeControl(MessageType::Hello, 1U), replayEmitter);
    (void)afterHello.HandleControl(bootstrap, replayEmitter);
    Require(
        afterHello.State() == AudioWorkerSessionState::Faulted && replayControl->openCalls.load() == 0,
        "replayed bootstrap after Hello must terminate the session without capture");
}

void TestServerProcessHandleAuthenticationWithFakePipe()
{
    LocalPipePair pipe = LocalPipePair::Create();
    HANDLE duplicatedProcess = nullptr;
    constexpr DWORD c_rights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    Require(
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &duplicatedProcess, c_rights, FALSE, 0U) != FALSE,
        "fake host should duplicate its own process with the worker rights");
    TestHandle process(duplicatedProcess);
    const DWORD processId = GetCurrentProcessId();
    const std::wstring imagePath = CurrentTestImagePath();
    Require(
        wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(pipe.Client(), processId, process.Get(), imagePath),
        "a connected fake pipe and matching retained process handle should authenticate");

    const DWORD wrongProcessId = processId == MAXDWORD ? processId - 1U : processId + 1U;
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(pipe.Client(), wrongProcessId, process.Get(), imagePath),
        "a pipe/handle PID mismatch must fail closed");
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(
            pipe.Client(), processId, process.Get(), L"C:\\missing-audio-test-service\\wslservice.exe"),
        "a process image other than the expected service image must fail closed");
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(
            pipe.Client(), processId, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(-1)), imagePath),
        "a pseudo process handle must fail before process queries");
    Require(
        !wsl::virtio_sound::audio_worker::windows::detail::ValidateServerProcessHandle(pipe.Client(), processId, nullptr, imagePath),
        "a null process handle must fail before process queries");
}
#endif

} // namespace

int main()
{
    try
    {
        TestConstructionAndHelloAreInert();
        TestLifecyclePcmAndShutdownOrdering();
        TestRestartNeedsNewEpochAndDropsOldQueuedPcm();
        TestWrongEpochStopFaultsAndStopsCapture();
        TestPcmPollCapsMessagesAt48000Frames();
        TestStaleEpochIsTerminalAndDoesNotReopen();
        TestOutOfOrderSequenceFaultsBeforeStart();
        TestOversizedDecodedMessageIsRejectedBeforeParsingOrStart();
        TestCaptureStartFailureMapsToFixedCodeAndPreservesEpoch();
        TestReadFailureDoesNotEmitQueuedOrFallbackPcm();
        TestStopFailureNeverAcknowledgesStopped();
        TestOverflowAndDiscontinuityFaultsAreExplicit();
        TestShutdownCaptureFailureNeverEmitsClosed();
        TestFailedStartedEmissionDisconnectsAndStops();
        TestDisconnectStopsWithoutReopening();
        TestSequenceArithmeticNeverWraps();
#if defined(_WIN32)
        TestServerIdentityBootstrapParsingAndReplay();
        TestServerProcessHandleAuthenticationWithFakePipe();
#endif
        std::cout << "AudioWorkerSession tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AudioWorkerSession test failure: " << error.what() << '\n';
        return 1;
    }
}
