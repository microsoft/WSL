// Copyright (C) Microsoft Corporation. All rights reserved.

#include "S16LePcmRing.hpp"
#include "VirtioSoundDevice.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wsl::virtio_sound::native;

namespace {

constexpr std::uint32_t c_statusOk = 0x8000U;
constexpr std::uint32_t c_statusIoError = 0x8003U;
constexpr std::uint32_t c_xrunEvent = 0x1101U;

struct FakeGuest
{
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(1U << 20U);
    std::uint32_t interruptCalls{};
    std::optional<std::uint64_t> failWriteAddress;
};

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool ReadGuest(void* context, std::uint64_t address, std::uint8_t* destination, std::size_t count)
{
    auto& guest = *static_cast<FakeGuest*>(context);
    if (address > guest.memory.size() || count > guest.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0U)
    {
        std::copy_n(guest.memory.data() + static_cast<std::size_t>(address), count, destination);
    }
    return true;
}

bool WriteGuest(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t count)
{
    auto& guest = *static_cast<FakeGuest*>(context);
    if (guest.failWriteAddress.has_value() && *guest.failWriteAddress == address)
    {
        return false;
    }
    if (address > guest.memory.size() || count > guest.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0U)
    {
        std::copy_n(source, count, guest.memory.data() + static_cast<std::size_t>(address));
    }
    return true;
}

bool DeliverInterrupt(void* context, std::uint64_t, std::uint32_t)
{
    ++static_cast<FakeGuest*>(context)->interruptCalls;
    return true;
}

void Put(FakeGuest& guest, std::uint64_t address, std::uint64_t value, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        guest.memory[static_cast<std::size_t>(address) + index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::uint64_t Get(const FakeGuest& guest, std::uint64_t address, std::size_t count)
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0; index < count; ++index)
    {
        value |= static_cast<std::uint64_t>(guest.memory[static_cast<std::size_t>(address) + index]) << (index * 8U);
    }
    return value;
}

void BarWrite(VirtioSoundDevice& device, std::uint64_t offset, std::uint64_t value, std::size_t width)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < width; ++index)
    {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
    device.WriteBar(offset, bytes.data(), width);
}

void ConfigureQueue(VirtioSoundDevice& device, std::uint16_t index, std::uint64_t descriptors, std::uint64_t available, std::uint64_t used)
{
    BarWrite(device, 22U, index, 2U);
    BarWrite(device, 32U, descriptors, 8U);
    BarWrite(device, 40U, available, 8U);
    BarWrite(device, 48U, used, 8U);
    BarWrite(device, 28U, 1U, 2U);
}

void Negotiate(VirtioSoundDevice& device)
{
    BarWrite(device, 0U, 1U, 4U);
    BarWrite(device, 8U, 1U, 4U);
    BarWrite(device, 12U, 1U, 4U);
    BarWrite(device, 20U, 1U, 1U);
    BarWrite(device, 20U, 3U, 1U);
    BarWrite(device, 20U, 11U, 1U);
    BarWrite(device, 20U, 15U, 1U);
}

void PutDescriptor(FakeGuest& guest, std::uint64_t table, std::uint16_t index, std::uint64_t address, std::uint32_t length, std::uint16_t flags, std::uint16_t next)
{
    const std::uint64_t descriptor = table + 16ULL * index;
    Put(guest, descriptor, address, 8U);
    Put(guest, descriptor + 8U, length, 4U);
    Put(guest, descriptor + 12U, flags, 2U);
    Put(guest, descriptor + 14U, next, 2U);
}

void PublishHead(FakeGuest& guest, std::uint64_t available, std::uint16_t ringSlot, std::uint16_t availableIndex, std::uint16_t head)
{
    Put(guest, available + 4U + 2ULL * ringSlot, head, 2U);
    Put(guest, available + 2U, availableIndex, 2U);
}

class TrackingPcmSource final : public PcmSource
{
public:
    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept override
    {
        ++readCalls;
        if (readiness != SourceReadiness::Ready || destination == nullptr || byteCount != static_cast<std::size_t>(frames) * 4U ||
            (availableFrames.has_value() && frames > *availableFrames))
        {
            return false;
        }
        std::fill(destination, destination + byteCount, sampleByte);
        if (availableFrames.has_value())
        {
            *availableFrames -= frames;
        }
        return true;
    }

    void Reset() noexcept override
    {
        ++resetCalls;
        readiness = SourceReadiness::Ready;
        if (availableFrames.has_value())
        {
            availableFrames = 0U;
        }
    }

    bool StartCapture(std::uint32_t periodFrames) noexcept override
    {
        ++startCalls;
        lastPeriodFrames = periodFrames;
        readiness = readinessOnStart;
        if (availableFrames.has_value())
        {
            availableFrames = 0U;
        }
        return startSucceeds;
    }

    void StopCapture() noexcept override
    {
        ++stopCalls;
        readiness = SourceReadiness::Pending;
        if (availableFrames.has_value())
        {
            availableFrames = 0U;
        }
    }

    SourceReadiness CaptureReadiness() const noexcept override
    {
        return readiness;
    }

    std::optional<std::uint32_t> AvailableFrames() const noexcept override
    {
        return availableFrames;
    }

    bool startSucceeds{true};
    SourceReadiness readinessOnStart{SourceReadiness::Ready};
    SourceReadiness readiness{SourceReadiness::Pending};
    std::optional<std::uint32_t> availableFrames;
    std::uint8_t sampleByte{0x5aU};
    std::uint32_t lastPeriodFrames{};
    std::uint32_t startCalls{};
    std::uint32_t stopCalls{};
    std::uint32_t resetCalls{};
    std::uint32_t readCalls{};
};

struct Fixture
{
    struct ControlDescriptor
    {
        std::uint64_t responseAddress{};
    };

    FakeGuest guest;
    std::shared_ptr<PcmSource> source;
    std::unique_ptr<VirtioSoundDevice> device;
    std::uint16_t nextControlAvailable{1U};

    explicit Fixture(std::shared_ptr<PcmSource> pcmSource) : source(std::move(pcmSource))
    {
        GuestMemoryCallbacks memory{&guest, &ReadGuest, &WriteGuest};
        InterruptCallbacks interrupts{&guest, &DeliverInterrupt, nullptr};
        device = std::make_unique<VirtioSoundDevice>(memory, interrupts, source);
        Negotiate(*device);
        ConfigureQueue(*device, 0U, 0x1000U, 0x2000U, 0x3000U);
        ConfigureQueue(*device, 1U, 0x4000U, 0x5000U, 0x6000U);
        ConfigureQueue(*device, 3U, 0x7000U, 0x8000U, 0x9000U);
    }

    ControlDescriptor QueueControl(const std::vector<std::uint8_t>& request)
    {
        const std::uint16_t ringSlot = static_cast<std::uint16_t>(nextControlAvailable - 1U);
        const std::uint16_t head = static_cast<std::uint16_t>(ringSlot * 2U);
        const std::uint64_t requestAddress = 0xa000U + 0x100ULL * ringSlot;
        const std::uint64_t responseAddress = requestAddress + 0x80U;
        std::copy(request.begin(), request.end(), guest.memory.begin() + static_cast<std::ptrdiff_t>(requestAddress));
        PutDescriptor(guest, 0x1000U, head, requestAddress, static_cast<std::uint32_t>(request.size()), 1U, static_cast<std::uint16_t>(head + 1U));
        PutDescriptor(guest, 0x1000U, static_cast<std::uint16_t>(head + 1U), responseAddress, 4U, 2U, 0U);
        PublishHead(guest, 0x2000U, ringSlot, nextControlAvailable, head);
        ++nextControlAvailable;
        return ControlDescriptor{responseAddress};
    }

    std::uint32_t SendControl(const std::vector<std::uint8_t>& request)
    {
        const ControlDescriptor descriptor = QueueControl(request);
        Require(device->ProcessControlQueue() == 1, "control command should complete");
        return static_cast<std::uint32_t>(Get(guest, descriptor.responseAddress, 4U));
    }

    std::uint32_t SetParams(std::uint32_t bufferBytes = 4096U, std::uint32_t periodBytes = 8U)
    {
        std::vector<std::uint8_t> request(24U, 0U);
        PutVector(request, 0U, 0x0101U, 4U);
        PutVector(request, 8U, bufferBytes, 4U);
        PutVector(request, 12U, periodBytes, 4U);
        PutVector(request, 16U, 1U << 4, 4U); // Enable PCM_XRUN notifications.
        request[20U] = 2U;
        request[21U] = 5U; // S16_LE
        request[22U] = 7U; // 48 kHz
        return SendControl(request);
    }

    std::uint32_t Command(std::uint32_t code)
    {
        std::vector<std::uint8_t> request(8U, 0U);
        PutVector(request, 0U, code, 4U);
        return SendControl(request);
    }

    ControlDescriptor QueueCommand(std::uint32_t code)
    {
        std::vector<std::uint8_t> request(8U, 0U);
        PutVector(request, 0U, code, 4U);
        return QueueControl(request);
    }

    void QueueRx(std::uint16_t availableIndex, std::uint32_t frames)
    {
        QueueRxAt(availableIndex, static_cast<std::uint16_t>(availableIndex - 1U), 0U, 0U, 0xc000U, 0xd000U, frames);
    }

    void QueueRxAt(
        std::uint16_t availableIndex,
        std::uint16_t ringSlot,
        std::uint16_t head,
        std::uint16_t descriptorIndex,
        std::uint64_t requestAddress,
        std::uint64_t responseAddress,
        std::uint32_t frames)
    {
        const std::uint32_t payloadBytes = frames * 4U;
        Put(guest, requestAddress, 0U, 4U);
        PutDescriptor(guest, 0x7000U, descriptorIndex, requestAddress, 4U, 1U, static_cast<std::uint16_t>(descriptorIndex + 1U));
        PutDescriptor(guest, 0x7000U, static_cast<std::uint16_t>(descriptorIndex + 1U), responseAddress, payloadBytes + 8U, 2U, 0U);
        PublishHead(guest, 0x8000U, ringSlot, availableIndex, head);
    }

    void QueueXrunEvent(std::uint16_t availableIndex)
    {
        PutDescriptor(guest, 0x4000U, 0U, 0xb000U, 8U, 2U, 0U);
        PublishHead(guest, 0x5000U, static_cast<std::uint16_t>(availableIndex - 1U), availableIndex, 0U);
    }

    static void PutVector(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
        }
    }
};

class LegacyResetSource final : public PcmSource
{
public:
    bool ReadFrames(std::uint32_t, std::uint8_t*, std::size_t) noexcept override
    {
        return true;
    }
    void Reset() noexcept override
    {
        ++resetCalls;
    }
    std::uint32_t resetCalls{};
};

void TestLegacySourceLifecycleDefaults()
{
    LegacyResetSource source;
    Require(source.CaptureReadiness() == SourceReadiness::Ready, "legacy sources remain immediately ready");
    Require(source.StartCapture(480U), "legacy source start defaults to a successful reset");
    source.StopCapture();
    Require(source.resetCalls == 2U, "legacy start and stop each map to exactly one reset");
}

void TestPendingReadinessDoesNotConsumeOrBankTime()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->readinessOnStart = SourceReadiness::Pending;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const auto start = fixture.QueueCommand(0x0104U);
    Put(fixture.guest, start.responseAddress, 0xdeadbeefU, 4U);
    Require(fixture.device->ProcessControlQueue() == 0, "pending START retains its control descriptor without replying");
    Require(
        source->startCalls == 1U && source->lastPeriodFrames == 2U,
        "START notifies the source with the negotiated period in frames");
    Require(fixture.device->CaptureRunning(), "guest stream remains logically running while source startup is pending");
    Require(
        Get(fixture.guest, 0x3002U, 2U) == 2U && Get(fixture.guest, start.responseAddress, 4U) == 0xdeadbeefU,
        "pending START leaves the used ring and response buffer untouched");

    fixture.QueueRx(1U, 2U);
    Require(
        fixture.device->ProcessControlQueue() == 0 && fixture.device->PumpCaptureElapsed(0U) == 0,
        "repeated queue and elapsed pumps keep the START pending");
    Require(fixture.device->PumpCaptureElapsed(1000000000U) == 0, "pending source discards elapsed frame credit");
    Require(
        fixture.device->PumpCapture(2) == 0 && source->readCalls == 0U,
        "pending START cannot read PCM or consume RX descriptors before its reply");
    Require(
        fixture.device->GetDiagnosticsSnapshot().m_rxAvailableProcessed == 0U && Get(fixture.guest, 0x9002U, 2U) == 0U &&
            source->startCalls == 1U && Get(fixture.guest, 0x3002U, 2U) == 2U,
        "pending pumps neither reissue START nor publish control or RX completion");

    source->readiness = SourceReadiness::Ready;
    Require(
        fixture.device->PumpCaptureElapsed(0U) == 0,
        "readiness completes START but a changed timer generation grants no stale credit");
    Require(
        Get(fixture.guest, start.responseAddress, 4U) == c_statusOk && Get(fixture.guest, 0x3002U, 2U) == 3U && source->startCalls == 1U,
        "one successful control response completes the retained START exactly once");
    Require(Get(fixture.guest, 0x9002U, 2U) == 0U, "zero elapsed credit leaves the RX descriptor untouched");
    Require(fixture.device->PumpCapture(2) == 2, "ready source consumes the retained RX descriptor");
    Require(
        source->readCalls == 1U && Get(fixture.guest, 0xd000U, 1U) == 0x5aU && Get(fixture.guest, 0xd008U, 4U) == c_statusOk,
        "ready source payload and successful status reach the guest");
}

void TestDeferredStartFailureRepliesAndStopsBeforeRx()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->readinessOnStart = SourceReadiness::Pending;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const auto start = fixture.QueueCommand(0x0104U);
    Require(fixture.device->ProcessControlQueue() == 0, "failed-start case retains START while source readiness is pending");

    fixture.QueueRx(1U, 2U);
    Put(fixture.guest, 0xd008U, 0xdeadbeefU, 4U);
    const std::uint32_t stopCallsBefore = source->stopCalls;
    source->readiness = SourceReadiness::Failed;
    Require(fixture.device->PumpCaptureElapsed(1000000000U) == 0, "elapsed pump resolves failed startup without capture");
    Require(
        Get(fixture.guest, start.responseAddress, 4U) == c_statusIoError && Get(fixture.guest, 0x3002U, 2U) == 3U &&
            !fixture.device->CaptureRunning() && source->stopCalls == stopCallsBefore + 1U,
        "failed readiness completes START once with IO_ERROR and stops the source");
    Require(
        fixture.device->PumpCapture(2) == 0 && fixture.device->ProcessControlQueue() == 0 && source->startCalls == 1U &&
            source->readCalls == 0U && Get(fixture.guest, 0x9002U, 2U) == 0U && Get(fixture.guest, 0xd008U, 4U) == 0xdeadbeefU,
        "failed START cannot consume RX or publish duplicate control completions");
}

void TestResetCancelsPendingStartWithoutStaleCompletion()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->readinessOnStart = SourceReadiness::Pending;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const auto start = fixture.QueueCommand(0x0104U);
    Put(fixture.guest, start.responseAddress, 0xdeadbeefU, 4U);
    Require(
        fixture.device->ProcessControlQueue() == 0 && Get(fixture.guest, 0x3002U, 2U) == 2U,
        "pending START is retained before reset");

    fixture.device->Reset();
    Require(
        fixture.device->ProcessControlQueue() == 0 && fixture.device->PumpCaptureElapsed(1000000000U) == 0 &&
            Get(fixture.guest, start.responseAddress, 4U) == 0xdeadbeefU && Get(fixture.guest, 0x3002U, 2U) == 2U && source->startCalls == 1U,
        "reset discards the retained chain without stale guest writes or repeat START");
}

void TestPendingStartGuestWriteFailureFaultsAndDropsChain()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->readinessOnStart = SourceReadiness::Pending;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const auto start = fixture.QueueCommand(0x0104U);
    Put(fixture.guest, start.responseAddress, 0xdeadbeefU, 4U);
    Require(fixture.device->ProcessControlQueue() == 0, "START is retained while readiness is pending");

    const std::uint32_t stopCallsBefore = source->stopCalls;
    source->readiness = SourceReadiness::Ready;
    fixture.guest.failWriteAddress = start.responseAddress;
    Require(fixture.device->PumpCaptureElapsed(0U) == 0, "timer pump contains a guest response write failure");
    fixture.guest.failWriteAddress.reset();
    Require(
        (fixture.device->DeviceStatus() & 64U) != 0U && source->stopCalls == stopCallsBefore + 1U &&
            Get(fixture.guest, start.responseAddress, 4U) == 0xdeadbeefU && Get(fixture.guest, 0x3002U, 2U) == 2U,
        "failed guest DMA faults and stops capture without publishing a used entry");
    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 0 && fixture.device->ProcessControlQueue() == 0 &&
            Get(fixture.guest, start.responseAddress, 4U) == 0xdeadbeefU && Get(fixture.guest, 0x3002U, 2U) == 2U && source->startCalls == 1U,
        "faulted timer cannot retry the consumed chain or issue another START");
}

void TestHardwareAvailabilityBypassesStartupCreditAndBoundsHugeElapsedTime()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->readinessOnStart = SourceReadiness::Pending;
    source->availableFrames = 0U;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const auto start = fixture.QueueCommand(0x0104U);
    Require(fixture.device->ProcessControlQueue() == 0, "hardware-paced START remains unacknowledged while capture is pending");

    fixture.QueueRxAt(1U, 0U, 0U, 0U, 0xc000U, 0xd000U, 2U);
    fixture.QueueRxAt(2U, 1U, 2U, 2U, 0xc100U, 0xd100U, 2U);
    source->readiness = SourceReadiness::Ready;
    source->availableFrames = 2U;

    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 2,
        "the first ready pump acknowledges START before delivering buffered frames");
    Require(
        source->readCalls == 1U && source->availableFrames == 0U && Get(fixture.guest, start.responseAddress, 4U) == c_statusOk &&
            Get(fixture.guest, 0x3002U, 2U) == 3U && Get(fixture.guest, 0x9002U, 2U) == 1U && Get(fixture.guest, 0xd008U, 4U) == c_statusOk,
        "one START response precedes one RX completion using exactly available frames");

    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 0 && source->readCalls == 1U && Get(fixture.guest, 0x9002U, 2U) == 1U,
        "large elapsed time cannot invent PCM or complete the second descriptor");

    source->availableFrames = 2U;
    Require(
        fixture.device->PumpCaptureElapsed(0U) == 2 && source->readCalls == 2U && Get(fixture.guest, 0x9002U, 2U) == 2U &&
            Get(fixture.guest, 0xd108U, 4U) == c_statusOk,
        "the next descriptor completes when its own hardware frames arrive");
}

void TestHardwareBurstLeavesOneGuestPeriodOfRingHeadroom()
{
    constexpr std::uint32_t frameBytes = 4U;
    constexpr std::uint32_t bufferFrames = 8192U;
    constexpr std::uint32_t periodFrames = 2048U;
    auto source = std::make_shared<TrackingPcmSource>();
    Fixture fixture(source);
    Require(
        fixture.SetParams(bufferFrames * frameBytes, periodFrames * frameBytes) == c_statusOk,
        "four-period SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    Require(fixture.Command(0x0104U) == c_statusOk, "START succeeds");

    for (std::uint16_t period = 0U; period < 4U; ++period)
    {
        const std::uint16_t availableIndex = static_cast<std::uint16_t>(period + 1U);
        const std::uint16_t ringSlot = period;
        const std::uint16_t head = static_cast<std::uint16_t>(period * 2U);
        const std::uint16_t descriptorIndex = head;
        const std::uint64_t requestAddress = 0xc000U + 0x10000ULL * period;
        const std::uint64_t responseAddress = 0xd000U + 0x10000ULL * period;
        fixture.QueueRxAt(
            availableIndex,
            ringSlot,
            head,
            descriptorIndex,
            requestAddress,
            responseAddress,
            periodFrames);
    }
    source->availableFrames = bufferFrames;

    const int deliveredFrames = fixture.device->PumpCaptureElapsed(0U);
    Require(
        deliveredFrames == static_cast<int>(bufferFrames - periodFrames),
        ("a source burst completes only the guest ring's free headroom, not an exact full-buffer wrap (got " +
         std::to_string(deliveredFrames) + " frames)").c_str());
    Require(
        source->readCalls == 3U && source->availableFrames == periodFrames && Get(fixture.guest, 0x9002U, 2U) == 3U,
        "three RX periods complete while the fourth remains queued with one period of source frames");
    Require(
        Get(fixture.guest, 0xd000U + 0x10000ULL * 2U + periodFrames * frameBytes, 4U) == c_statusOk &&
            Get(fixture.guest, 0xd000U + 0x10000ULL * 3U + periodFrames * frameBytes, 4U) == 0U,
        "headroom completion publishes the third status and leaves the fourth descriptor untouched");
}

void TestHardwareBufferCapacityMatchesPumpCadence()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->availableFrames = 0U;
    Fixture fixture(source);
    Require(fixture.SetParams(512U * 4U, 128U * 4U) == 0x8002U,
        "384 frames per tick cannot sustain 480 frames of capture");
    Require(fixture.SetParams(640U * 4U, 192U * 4U) == 0x8002U,
        "capacity is rounded down to complete guest periods");
    Require(fixture.SetParams(512U * 4U, 512U * 4U) == 0x8002U,
        "one-period buffers cannot preserve guest-ring headroom");
    Require(source->startCalls == 0U, "unsupported parameters never start capture");
    Require(fixture.SetParams(1024U * 4U, 256U * 4U) == c_statusOk,
        "768 frames per tick can sustain the nominal capture rate");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    Require(fixture.Command(0x0104U) == c_statusOk, "START succeeds");

    std::uint16_t availableIndex = 0U;
    std::uint32_t totalFrames = 0U;
    for (unsigned int tick = 0U; tick < 100U; ++tick)
    {
        source->availableFrames = source->availableFrames.value() + 480U;
        const auto periods = source->availableFrames.value() / 256U;
        for (std::uint32_t period = 0U; period < periods; ++period)
        {
            const auto slot = static_cast<std::uint16_t>(availableIndex % VirtioSoundDevice::c_maxQueueSize);
            ++availableIndex;
            const auto descriptorSlot = static_cast<std::uint16_t>(slot % 8U);
            const auto head = static_cast<std::uint16_t>(descriptorSlot * 2U);
            fixture.QueueRxAt(availableIndex, slot, head, head,
                0xc000U + 0x1000ULL * descriptorSlot, 0x20000U + 0x1000ULL * descriptorSlot, 256U);
        }
        const int delivered = fixture.device->PumpCaptureElapsed(10000000U);
        Require(delivered == static_cast<int>(periods * 256U) && delivered <= 768,
            "each tick drains complete periods within one-period headroom");
        totalFrames += static_cast<std::uint32_t>(delivered);
        Require(source->availableFrames.value() < 256U,
            "repeated pumps leave only a partial period, not growing backlog");
    }
    Require(totalFrames + source->availableFrames.value() == 48000U,
        "one second of clocked input is accounted for without loss");

    Fixture synthetic(nullptr);
    Require(synthetic.SetParams(512U * 4U, 128U * 4U) == c_statusOk,
        "source-clock capacity policy does not restrict synthetic input");
}

void TestHardwareAvailabilityLeavesZeroAndPartialDescriptorsQueued()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->availableFrames = 0U;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    Require(fixture.Command(0x0104U) == c_statusOk, "START succeeds");
    fixture.QueueRx(1U, 2U);

    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 0 && source->readCalls == 0U && Get(fixture.guest, 0x9002U, 2U) == 0U,
        "zero available frames leave the RX descriptor untouched");

    source->availableFrames = 1U;
    Require(
        fixture.device->PumpCapture(1000) == 0 && source->readCalls == 0U && Get(fixture.guest, 0x9002U, 2U) == 0U,
        "partial availability cannot trigger an underflowing direct pump");

    source->availableFrames = 2U;
    Require(
        fixture.device->PumpCapture(1000) == 2 && source->readCalls == 1U && source->availableFrames == 0U &&
            Get(fixture.guest, 0x9002U, 2U) == 1U && Get(fixture.guest, 0xd008U, 4U) == c_statusOk,
        "direct pumping is capped to the complete frames currently available");
}

void TestHardwareAvailabilityDoesNotSurviveStopOrReset()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->availableFrames = 2U;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    Require(fixture.Command(0x0104U) == c_statusOk, "START succeeds");
    fixture.QueueRx(1U, 2U);

    Require(
        fixture.Command(0x0105U) == c_statusOk && source->availableFrames == 0U, "STOP invalidates buffered-frame availability");
    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 0 && fixture.device->PumpCapture(2) == 0 && source->readCalls == 0U &&
            Get(fixture.guest, 0x9002U, 2U) == 0U,
        "stopped capture cannot deliver stale frames");

    Require(
        fixture.Command(0x0104U) == c_statusOk && source->availableFrames == 0U,
        "restart begins with no frames from the previous capture epoch");
    Require(
        fixture.device->PumpCaptureElapsed(1000000000U) == 0 && source->readCalls == 0U,
        "elapsed time cannot revive old frames after restart");

    source->availableFrames = 2U;
    Require(fixture.device->PumpCaptureElapsed(0U) == 2 && source->readCalls == 1U, "fresh frames can be delivered after restart");
    source->availableFrames = 2U;
    fixture.device->Reset();
    Require(
        source->availableFrames == 0U && !fixture.device->CaptureRunning() && fixture.device->PumpCapture(2) == 0 && source->readCalls == 1U,
        "device reset clears source availability and stops further reads");
}

void TestStartAndAsyncReadinessFailuresSurfaceErrors()
{
    auto source = std::make_shared<TrackingPcmSource>();
    source->startSucceeds = false;
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    const std::uint32_t stopCallsBeforeFailure = source->stopCalls;
    Require(fixture.Command(0x0104U) == c_statusIoError, "synchronous source start failure returns IO_ERROR");
    Require(
        !fixture.device->CaptureRunning() && source->startCalls == 1U && source->stopCalls == stopCallsBeforeFailure + 1U,
        "failed START is cleaned up and never enters Running");

    source->startSucceeds = true;
    source->readinessOnStart = SourceReadiness::Ready;
    Require(fixture.Command(0x0104U) == c_statusOk, "ready source start is acknowledged immediately");
    source->readiness = SourceReadiness::Failed;
    fixture.QueueRx(1U, 2U);
    fixture.QueueXrunEvent(1U);
    Require(fixture.device->PumpCapture(2) == 0, "failed asynchronous source emits no PCM frames");
    Require(
        source->readCalls == 0U && Get(fixture.guest, 0xd008U, 4U) == c_statusIoError && Get(fixture.guest, 0x9008U, 4U) == 8U,
        "failed readiness completes RX with IO_ERROR and no payload");
    Require(
        Get(fixture.guest, 0xb000U, 4U) == c_xrunEvent && Get(fixture.guest, 0x6002U, 2U) == 1U,
        "failed readiness emits one PCM_XRUN event when negotiated");

    fixture.QueueRx(2U, 2U);
    fixture.QueueXrunEvent(2U);
    Require(
        fixture.device->PumpCapture(2) == 0 && Get(fixture.guest, 0x9002U, 2U) == 2U && Get(fixture.guest, 0x6002U, 2U) == 1U,
        "persistent source failure reports later RX buffers without duplicating XRUN");
}

void TestStopRestartDiscardsOldRingFrames()
{
    auto source = std::make_shared<S16LePcmRingSource>(PcmFormat{}, 16U);
    Fixture fixture(source);
    Require(fixture.SetParams() == c_statusOk, "SET_PARAMS succeeds");
    Require(fixture.Command(0x0102U) == c_statusOk, "PREPARE succeeds");
    Require(fixture.Command(0x0104U) == c_statusOk, "first START succeeds");

    const std::array<std::uint8_t, 8> oldFrames{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    const std::uint64_t oldGeneration = source->Generation();
    Require(
        source->TryWriteFrames(oldGeneration, oldFrames.data(), 2U, oldFrames.size()) == PcmRingWriteResult::Written,
        "old-epoch PCM can be queued");
    Require(fixture.Command(0x0105U) == c_statusOk, "STOP acknowledges after source invalidation");
    Require(fixture.Command(0x0104U) == c_statusOk, "immediate restart from Stopped is accepted");

    const std::array<std::uint8_t, 8> newFrames{21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U};
    const std::uint64_t newGeneration = source->Generation();
    Require(
        source->TryWriteFrames(oldGeneration, oldFrames.data(), 2U, oldFrames.size()) == PcmRingWriteResult::StaleGeneration,
        "a producer holding the prior generation cannot write after restart");
    Require(
        source->TryWriteFrames(newGeneration, newFrames.data(), 2U, newFrames.size()) == PcmRingWriteResult::Written,
        "new-epoch PCM can be queued");

    fixture.QueueRx(1U, 2U);
    Require(fixture.device->PumpCapture(2) == 2, "restarted source supplies a complete capture buffer");
    Require(
        std::equal(newFrames.begin(), newFrames.end(), fixture.guest.memory.begin() + 0xd000) && Get(fixture.guest, 0xd008U, 4U) == c_statusOk,
        "restart delivers only new-epoch PCM and never old queued frames");
}

void TestPrepareParamsResetFaultAndDestructionNotifySource()
{
    auto source = std::make_shared<TrackingPcmSource>();
    Fixture fixture(source);
    const std::uint32_t initialStops = source->stopCalls;
    Require(
        fixture.SetParams() == c_statusOk && source->stopCalls == initialStops + 1U,
        "SET_PARAMS invalidates the source exactly once");
    Require(
        fixture.Command(0x0102U) == c_statusOk && source->stopCalls == initialStops + 2U,
        "PREPARE invalidates the source exactly once");
    Require(
        fixture.Command(0x0104U) == c_statusOk && source->startCalls == 1U,
        "START activates the source once without an extra Reset/Stop");

    Require(
        fixture.Command(0x0105U) == c_statusOk && source->stopCalls == initialStops + 3U,
        "STOP immediately invalidates the active source");
    Require(fixture.Command(0x0104U) == c_statusOk && source->startCalls == 2U, "short restart notifies the source once");
    Require(fixture.Command(0x0105U) == c_statusOk, "second STOP succeeds");
    Require(
        fixture.Command(0x0103U) == c_statusOk && source->stopCalls == initialStops + 5U,
        "RELEASE invalidates the source after STOP");
    Require(
        fixture.SetParams() == c_statusOk && source->stopCalls == initialStops + 6U,
        "reconfiguration invalidates the source exactly once");
    Require(
        fixture.Command(0x0102U) == c_statusOk && source->stopCalls == initialStops + 7U,
        "re-prepare invalidates the source exactly once");
    Require(fixture.Command(0x0104U) == c_statusOk, "source can start after re-prepare");

    fixture.device->Reset();
    Require(
        source->stopCalls == initialStops + 8U && !fixture.device->CaptureRunning() && fixture.device->DeviceStatus() == 0U,
        "device Reset stops active capture and clears the stream");

    auto faultSource = std::make_shared<TrackingPcmSource>();
    Fixture faultFixture(faultSource);
    (void)faultFixture.SetParams();
    (void)faultFixture.Command(0x0102U);
    (void)faultFixture.Command(0x0104U);
    const std::uint32_t stopsBeforeFault = faultSource->stopCalls;
    BarWrite(*faultFixture.device, 20U, 7U, 1U); // Illegally clear DRIVER_OK.
    Require(
        (faultFixture.device->DeviceStatus() & 64U) != 0U && faultSource->stopCalls == stopsBeforeFault + 1U &&
            !faultFixture.device->CaptureRunning(),
        "fatal device fault stops active source until reset");

    auto destructorSource = std::make_shared<TrackingPcmSource>();
    std::uint32_t stopsBeforeDestruction{};
    {
        Fixture destructorFixture(destructorSource);
        (void)destructorFixture.SetParams();
        (void)destructorFixture.Command(0x0102U);
        (void)destructorFixture.Command(0x0104U);
        stopsBeforeDestruction = destructorSource->stopCalls;
    }
    Require(destructorSource->stopCalls == stopsBeforeDestruction + 1U, "destroying an active device stops the source");
}

} // namespace

int main()
{
    try
    {
        TestLegacySourceLifecycleDefaults();
        TestPendingReadinessDoesNotConsumeOrBankTime();
        TestDeferredStartFailureRepliesAndStopsBeforeRx();
        TestResetCancelsPendingStartWithoutStaleCompletion();
        TestPendingStartGuestWriteFailureFaultsAndDropsChain();
        TestHardwareAvailabilityBypassesStartupCreditAndBoundsHugeElapsedTime();
        TestHardwareBurstLeavesOneGuestPeriodOfRingHeadroom();
        TestHardwareBufferCapacityMatchesPumpCadence();
        TestHardwareAvailabilityLeavesZeroAndPartialDescriptorsQueued();
        TestHardwareAvailabilityDoesNotSurviveStopOrReset();
        TestStartAndAsyncReadinessFailuresSurfaceErrors();
        TestStopRestartDiscardsOldRingFrames();
        TestPrepareParamsResetFaultAndDestructionNotifySource();
        std::cout << "PCM source lifecycle tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "PCM source lifecycle test failure: " << error.what() << '\n';
        return 1;
    }
}
