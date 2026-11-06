// Copyright (C) Microsoft Corporation. All rights reserved.

#include "VirtioSoundDevice.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using wsl::virtio_sound::native::DiagnosticSnapshot;
using wsl::virtio_sound::native::GuestMemoryCallbacks;
using wsl::virtio_sound::native::InterruptCallbacks;
using wsl::virtio_sound::native::PcmFormat;
using wsl::virtio_sound::native::PcmSource;
using wsl::virtio_sound::native::VirtioSoundDevice;

namespace {
struct FakeHost
{
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(1 << 20);
    int guestReadCalls{};
    int guestWriteCalls{};
    int msiCalls{};
    std::uint64_t msiAddress{};
    std::uint32_t msiData{};
    int assertions{};
    bool observeRxPublicationAtMsi{};
    bool msiObservedExpectedRxPublication{};
    std::uint64_t expectedUsedIndexAddress{};
    std::uint64_t expectedUsedEntryAddress{};
    std::uint16_t expectedUsedIndex{};
    std::uint32_t expectedUsedId{};
    std::uint32_t expectedUsedLength{};
    bool failGuestWrite{};
    std::uint64_t failGuestWriteAddress{};
    VirtioSoundDevice* queueEnableProbeDevice{};
    std::uint64_t queueEnableProbeAddress{};
    bool rxFlagsWriteObserved{};
    bool rxFlagsWriteSawQueueDisabled{};
};

class FailingPcmSource final : public PcmSource
{
public:
    bool ReadFrames(std::uint32_t, std::uint8_t*, std::size_t) noexcept override
    {
        ++readCalls;
        return false;
    }
    void Reset() noexcept override
    {
        ++resetCalls;
    }
    int readCalls{};
    int resetCalls{};
};

class CountingPcmSource final : public PcmSource
{
public:
    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept override
    {
        ++readCalls;
        if (destination == nullptr || byteCount != static_cast<std::size_t>(frames) * 4)
        {
            return false;
        }
        std::fill_n(destination, byteCount, static_cast<std::uint8_t>(0x5a));
        return true;
    }
    void Reset() noexcept override
    {
        ++resetCalls;
    }
    int readCalls{};
    int resetCalls{};
};

void Assert(bool condition, const char* message, FakeHost& host)
{
    ++host.assertions;
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void ClearGuestMemory(FakeHost& host)
{
    std::fill(host.memory.begin(), host.memory.end(), static_cast<std::uint8_t>(0));
}

bool Read(void* context, std::uint64_t address, std::uint8_t* destination, std::size_t count)
{
    auto& host = *static_cast<FakeHost*>(context);
    ++host.guestReadCalls;
    if (address > host.memory.size() || count > host.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0)
    {
        std::copy_n(host.memory.data() + static_cast<std::size_t>(address), count, destination);
    }
    return true;
}

bool Write(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t count)
{
    auto& host = *static_cast<FakeHost*>(context);
    ++host.guestWriteCalls;
    if (host.queueEnableProbeDevice != nullptr && address == host.queueEnableProbeAddress && count == 2)
    {
        std::array<std::uint8_t, 2> queueEnabled{};
        host.queueEnableProbeDevice->ReadBar(28, queueEnabled.data(), queueEnabled.size());
        host.rxFlagsWriteObserved = true;
        host.rxFlagsWriteSawQueueDisabled = queueEnabled[0] == 0 && queueEnabled[1] == 0;
    }
    if (host.failGuestWrite && address == host.failGuestWriteAddress)
    {
        return false;
    }
    if (address > host.memory.size() || count > host.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0)
    {
        std::copy_n(source, count, host.memory.data() + static_cast<std::size_t>(address));
    }
    return true;
}

bool Msi(void* context, std::uint64_t address, std::uint32_t data)
{
    auto& host = *static_cast<FakeHost*>(context);
    ++host.msiCalls;
    host.msiAddress = address;
    host.msiData = data;
    if (host.observeRxPublicationAtMsi)
    {
        const auto readLittleEndian = [&host](std::uint64_t location, std::size_t length) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < length; ++i)
            {
                value |= static_cast<std::uint64_t>(host.memory[static_cast<std::size_t>(location) + i]) << (8 * i);
            }
            return value;
        };
        host.msiObservedExpectedRxPublication = readLittleEndian(host.expectedUsedIndexAddress, 2) == host.expectedUsedIndex &&
                                                readLittleEndian(host.expectedUsedEntryAddress, 4) == host.expectedUsedId &&
                                                readLittleEndian(host.expectedUsedEntryAddress + 4, 4) == host.expectedUsedLength;
    }
    return true;
}

void Put(FakeHost& host, std::uint64_t address, std::uint64_t value, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i)
    {
        host.memory[static_cast<std::size_t>(address) + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::uint64_t Get(const FakeHost& host, std::uint64_t address, std::size_t length)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < length; ++i)
    {
        value |= static_cast<std::uint64_t>(host.memory[static_cast<std::size_t>(address) + i]) << (8 * i);
    }
    return value;
}

void BarWrite(VirtioSoundDevice& device, std::uint64_t offset, std::uint64_t value, std::size_t width)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < width; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    device.WriteBar(offset, bytes.data(), width);
}

void ConfigureQueue(VirtioSoundDevice& device, std::uint16_t index, std::uint64_t descriptor, std::uint64_t available, std::uint64_t used, std::uint16_t vector = 0xffff)
{
    BarWrite(device, 22, index, 2);
    BarWrite(device, 26, vector, 2);
    BarWrite(device, 32, descriptor, 8);
    BarWrite(device, 40, available, 8);
    BarWrite(device, 48, used, 8);
    BarWrite(device, 28, 1, 2);
}

void Negotiate(VirtioSoundDevice& device)
{
    BarWrite(device, 0, 1, 4);  // device feature page 1
    BarWrite(device, 8, 1, 4);  // driver feature page 1
    BarWrite(device, 12, 1, 4); // VIRTIO_F_VERSION_1
    BarWrite(device, 20, 1, 1);
    BarWrite(device, 20, 3, 1);
    BarWrite(device, 20, 11, 1);
    BarWrite(device, 20, 15, 1);
}

void PutDescriptor(FakeHost& host, std::uint64_t table, std::uint16_t index, std::uint64_t address, std::uint32_t length, std::uint16_t flags, std::uint16_t next)
{
    std::uint64_t at = table + 16ULL * index;
    Put(host, at, address, 8);
    Put(host, at + 8, length, 4);
    Put(host, at + 12, flags, 2);
    Put(host, at + 14, next, 2);
}

void PublishHead(FakeHost& host, std::uint64_t available, std::uint16_t ringSlot, std::uint16_t availableIndex, std::uint16_t head)
{
    Put(host, available + 4 + 2ULL * ringSlot, head, 2);
    Put(host, available + 2, availableIndex, 2);
}

std::unique_ptr<VirtioSoundDevice> Create(FakeHost& host)
{
    GuestMemoryCallbacks memory{&host, &Read, &Write};
    InterruptCallbacks interrupts{&host, &Msi, nullptr};
    return std::make_unique<VirtioSoundDevice>(memory, interrupts);
}

void TestPciAndControl(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Assert(device->ReadConfig(0, 4) == 0x10591af4, "Virtio-sound PCI identity", host);
    Assert(device->ReadConfig(0x34, 1) == 0x40, "PCI capability list starts at 0x40", host);
    Assert(device->ReadConfig(0x40, 4) == 0x01105009, "Common configuration vendor capability", host);
    Assert(Get(host, 0, 1) == 0, "Fresh fake guest memory is empty", host);

    Negotiate(*device);
    Assert(device->DeviceStatus() == 15, "Version 1 feature negotiation succeeds", host);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000);
    std::array<std::uint8_t, 16> request{};
    request[0] = 0x00;
    request[1] = 0x01; // PCM_INFO
    request[8] = 1;    // one stream
    request[12] = 32;  // virtio_snd_pcm_info size
    Write(&host, 0x4000, request.data(), request.size());
    PutDescriptor(host, 0x1000, 0, 0x4000, 16, 1, 1);
    PutDescriptor(host, 0x1000, 1, 0x5000, 36, 2, 0);
    PublishHead(host, 0x2000, 0, 1, 0);
    Assert(device->ProcessControlQueue() == 1, "Valid PCM_INFO descriptor chain completes", host);
    Assert(Get(host, 0x5000, 4) == 0x8000, "PCM_INFO reports success", host);
    Assert(Get(host, 0x5000 + 8, 4) == 16 && Get(host, 0x5000 + 29, 2) == 0x0202, "PCM_INFO advertises XRUN and stereo capture", host);
    Assert(
        Get(host, 0x3002, 2) == 1 && Get(host, 0x3008, 4) == 36, "Control completion publishes the expected used index and length", host);

    // The request has the PCM_INFO code but is missing its 12-byte query tail.
    std::array<std::uint8_t, 8> shortInfo{};
    shortInfo[0] = 0x00;
    shortInfo[1] = 0x01;
    Write(&host, 0x4100, shortInfo.data(), shortInfo.size());
    PutDescriptor(host, 0x1000, 0, 0x4100, 8, 1, 1);
    PutDescriptor(host, 0x1000, 1, 0x5000, 36, 2, 0);
    PublishHead(host, 0x2000, 1, 2, 0);
    Assert(device->ProcessControlQueue() == 1, "Malformed control request receives a response", host);
    Assert(
        Get(host, 0x5000, 4) == 0x8001 && Get(host, 0x3008 + 8, 4) == 4,
        "Malformed PCM_INFO returns BAD_MSG and only four used bytes",
        host);
    Assert(device->DeviceStatus() == 15, "Malformed control request does not fault transport", host);
}

void TestMalformedDescriptorsAndReset(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Negotiate(*device);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000);
    PutDescriptor(host, 0x1000, 0, 0x4000, 4, 1, 0); // cyclic NEXT -> itself
    Put(host, 0x4000, 0x100, 4);
    PublishHead(host, 0x2000, 0, 1, 0);
    Assert(device->ProcessControlQueue() == 0, "Cyclic chain is not completed", host);
    Assert(
        (device->DeviceStatus() & 64) != 0 && device->LastFault().find("cyclic") != std::string::npos,
        "Cyclic chain sets NEEDS_RESET with a useful fault",
        host);
    Assert(Get(host, 0x3002, 2) == 0, "Malformed chain does not publish a used entry", host);

    device->Reset();
    Assert(device->DeviceStatus() == 0, "Reset clears device status", host);
    std::array<std::uint8_t, 2> common{};
    device->ReadBar(24, common.data(), common.size());
    Assert(common[0] == 0x80 && common[1] == 0, "Reset restores the default queue size", host);
}

void TestAddressOverflowAndMsix(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Negotiate(*device);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000, 2);
    PutDescriptor(host, 0x1000, 0, UINT64_MAX - 1, 4, 0, 0);
    PublishHead(host, 0x2000, 0, 1, 0);
    Assert(
        device->ProcessControlQueue() == 0 && (device->DeviceStatus() & 64) != 0,
        "Guest address overflow faults before host memory access",
        host);

    device->Reset();
    Negotiate(*device);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000, 2);
    device->WriteConfig(0x86, 0xc004, 2); // MSI-X enabled and function masked
    std::array<std::uint8_t, 8> shortInfo{};
    shortInfo[0] = 0x00;
    shortInfo[1] = 0x01;
    Write(&host, 0x4100, shortInfo.data(), shortInfo.size());
    PutDescriptor(host, 0x1000, 0, 0x4100, 8, 0, 0);
    PublishHead(host, 0x2000, 0, 1, 0);
    Assert(device->ProcessControlQueue() == 1, "MSI-X test completion is published", host);
    std::array<std::uint8_t, 8> pba{};
    device->ReadBar(0x1800, pba.data(), pba.size());
    Assert(
        host.msiCalls == 0 &&
            [&pba] {
                std::uint64_t value = 0;
                for (int i = 0; i < 8; ++i)
                {
                    value |= static_cast<std::uint64_t>(pba[i]) << (8 * i);
                }
                return value;
            }() == (1ULL << 2),
        "Function-masked vector remains pending in PBA",
        host);
    BarWrite(*device, 0x1020, 0xfee00000, 8);
    BarWrite(*device, 0x1028, 0x55, 4);
    BarWrite(*device, 0x102c, 0, 4);
    device->WriteConfig(0x86, 0x8004, 2); // unmask function
    Assert(
        host.msiCalls == 1 && host.msiAddress == 0xfee00000 && host.msiData == 0x55, "Unmask delivers pending MSI-X address/data", host);
    device->ReadBar(0x1800, pba.data(), pba.size());
    Assert(std::all_of(pba.begin(), pba.end(), [](std::uint8_t value) { return value == 0; }), "Delivered MSI-X vector clears PBA", host);

    DiagnosticSnapshot before = device->GetDiagnosticsSnapshot();
    device->Reset();
    DiagnosticSnapshot after = device->GetDiagnosticsSnapshot();
    Assert(
        after.m_resetCount == before.m_resetCount + 1 && device->DeviceStatus() == 0,
        "Queue reset increments diagnostics and clears negotiated state",
        host);
    std::array<std::uint8_t, 4> readback{};
    device->ReadBar(24, readback.data(), 2);
    Assert(readback[0] == 0x80 && readback[1] == 0, "Reset restores default queue size and disables the queue", host);
}

void SubmitControl(FakeHost& host, VirtioSoundDevice& device, std::uint16_t availableIndex, const std::uint8_t* request, std::size_t requestLength)
{
    Write(&host, 0x4000, request, requestLength);
    PutDescriptor(host, 0x1000, 0, 0x4000, static_cast<std::uint32_t>(requestLength), 1, 1);
    PutDescriptor(host, 0x1000, 1, 0x5000, 4, 2, 0);
    PublishHead(host, 0x2000, static_cast<std::uint16_t>(availableIndex - 1), availableIndex, 0);
    Assert(device.ProcessControlQueue() == 1, "PCM control command completes", host);
}

void TestFormatAndSyntheticCapture(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Negotiate(*device);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000);

    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1; // PCM_SET_PARAMS
    setParams[1] = 1;
    setParams[9] = 0x1e;  // 7680 buffer bytes: 1920 stereo frames
    setParams[13] = 0x1e; // one 1920-frame period
    setParams[20] = 1;    // mono is unsupported by the synthetic default
    setParams[21] = 5;    // S16_LE
    setParams[22] = 7;    // 48 kHz
    SubmitControl(host, *device, 1, setParams.data(), setParams.size());
    Assert(Get(host, 0x5000, 4) == 0x8002, "Synthetic backend rejects a format it cannot provide", host);

    setParams[20] = 2;
    SubmitControl(host, *device, 2, setParams.data(), setParams.size());
    Assert(Get(host, 0x5000, 4) == 0x8000, "Supported S16 stereo 48 kHz format negotiates", host);
    std::array<std::uint8_t, 8> command{};
    command[0] = 2; // PREPARE
    command[1] = 1;
    SubmitControl(host, *device, 3, command.data(), command.size());
    command[0] = 4; // START
    command[1] = 1;
    SubmitControl(host, *device, 4, command.data(), command.size());

    ConfigureQueue(*device, 3, 0x7000, 0x8000, 0x9000);
    Put(host, 0xa000, 0, 4); // stream 0
    PutDescriptor(host, 0x7000, 0, 0xa000, 4, 1, 1);
    PutDescriptor(host, 0x7000, 1, 0xb000, 7688, 2, 0); // 1920 frames + status
    PublishHead(host, 0x8000, 0, 1, 0);
    Assert(
        device->CaptureRunning() && device->CapturePeriodFrames() == 1920 &&
            device->MaximumCaptureFrameBudget() == VirtioSoundDevice::c_maxTransferBytes / 4,
        "Capture pacing reports its active stream, period, and transport bound",
        host);
    Assert(device->PumpCaptureElapsed(0) == 0, "Elapsed pump observes START generation without stale credit", host);
    for (int tick = 0; tick < 3; ++tick)
    {
        Assert(device->PumpCaptureElapsed(10000000) == 0, "An incomplete 1920-frame period retains elapsed credit", host);
    }
    Assert(device->PumpCaptureElapsed(10000000) == 1920, "Four 10 ms intervals complete a 1920-frame period", host);
    const std::array<std::uint8_t, 8> expected{0x00, 0xc0, 0x00, 0x40, 0x01, 0xc0, 0xff, 0x3f};
    Assert(
        std::equal(expected.begin(), expected.end(), host.memory.begin() + 0xb000),
        "Synthetic PCM retains the exact ascending/negated S16 ramp",
        host);
    Assert(
        Get(host, 0xb000 + 7680, 4) == 0x8000 && Get(host, 0x9008, 4) == 7688,
        "Capture appends PCM status and publishes the full used length",
        host);

    Put(host, 0xa004, 0, 4);
    PutDescriptor(host, 0x7000, 2, 0xa004, 4, 1, 3);
    PutDescriptor(host, 0x7000, 3, 0xc000, 7688, 2, 0);
    PublishHead(host, 0x8000, 1, 2, 2);
    Assert(
        device->PumpCaptureElapsed(0) == 0 && device->PumpCaptureElapsed(10000000) == 0,
        "A second period starts with zero consumed-credit debt",
        host);
    command[0] = 5; // STOP between timer polls
    command[1] = 1;
    SubmitControl(host, *device, 5, command.data(), command.size());
    Assert(device->PumpCaptureElapsed(10000000) == 0 && !device->CaptureRunning(), "STOP clears pending elapsed credit", host);
    command[0] = 4; // START between timer polls
    SubmitControl(host, *device, 6, command.data(), command.size());
    Assert(device->PumpCaptureElapsed(10000000) == 0, "START generation does not inherit paused-time credit", host);
    for (int tick = 0; tick < 3; ++tick)
    {
        Assert(device->PumpCaptureElapsed(10000000) == 0, "Restarted stream accumulates fresh frame credit", host);
    }
    Assert(device->PumpCaptureElapsed(10000000) == 1920, "Restarted stream completes only after four fresh 10 ms intervals", host);
    device->Reset();
    Assert(
        !device->CaptureRunning() && device->PumpCaptureElapsed(10000000) == 0,
        "Device reset clears active stream state and elapsed frame credit",
        host);
}

void ConfigureCacheTestStream(FakeHost& host, VirtioSoundDevice& device)
{
    Negotiate(device);
    ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1; // PCM_SET_PARAMS
    setParams[1] = 1;
    setParams[9] = 0x1e;  // 7680-byte buffer
    setParams[13] = 0x1e; // 1920-frame period
    setParams[20] = 2;    // stereo
    setParams[21] = 5;    // S16_LE
    setParams[22] = 7;    // 48 kHz
    SubmitControl(host, device, 1, setParams.data(), setParams.size());
    std::array<std::uint8_t, 8> command{};
    command[0] = 2; // PREPARE
    command[1] = 1;
    SubmitControl(host, device, 2, command.data(), command.size());
    command[0] = 4; // START
    SubmitControl(host, device, 3, command.data(), command.size());
    ConfigureQueue(device, 3, 0x7000, 0x8000, 0x9000);
}

void TestPendingRxChainCacheAndReset(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    ConfigureCacheTestStream(host, *device);

    Put(host, 0xa000, 0, 4);
    PutDescriptor(host, 0x7000, 0, 0xa000, 4, 1, 1);
    PutDescriptor(host, 0x7000, 1, 0xb000, 7688, 2, 0);
    PublishHead(host, 0x8000, 0, 1, 0);
    const int readsBeforePeek = host.guestReadCalls;
    Assert(device->PumpCapture(0) == 0, "An undersized budget retains the first RX chain", host);
    const int readsAfterPeek = host.guestReadCalls;
    Assert(readsAfterPeek > readsBeforePeek, "The first pending RX chain is inspected from guest memory", host);
    Assert(device->PumpCapture(0) == 0, "A repeated undersized budget leaves the RX chain pending", host);
    Assert(host.guestReadCalls == readsAfterPeek, "A cached pending RX chain causes no repeat guest-memory reads", host);
    Assert(device->PumpCapture(1920) == 1920, "A sufficient budget completes the cached RX chain", host);
    const std::array<std::uint8_t, 8> expected{0x00, 0xc0, 0x00, 0x40, 0x01, 0xc0, 0xff, 0x3f};
    Assert(
        std::equal(expected.begin(), expected.end(), host.memory.begin() + 0xb000), "Cached chain receives the exact expected PCM", host);
    Assert(
        Get(host, 0xb000 + 7680, 4) == 0x8000 && Get(host, 0x9008, 4) == 7688,
        "Cached chain publishes the normal status and used length",
        host);

    // Cache a different outstanding descriptor, then reset and point the new
    // queue at another buffer. Reusing the pre-reset cache would write b800.
    Put(host, 0xa004, 0, 4);
    PutDescriptor(host, 0x7000, 2, 0xa004, 4, 1, 3);
    PutDescriptor(host, 0x7000, 3, 0xb800, 7688, 2, 0);
    PublishHead(host, 0x8000, 1, 2, 2);
    Assert(device->PumpCapture(0) == 0, "The second RX chain is cached before reset", host);

    device->Reset();
    ClearGuestMemory(host);
    ConfigureCacheTestStream(host, *device);
    std::fill_n(host.memory.begin() + 0xb800, 7688, static_cast<std::uint8_t>(0xcc));
    std::fill_n(host.memory.begin() + 0xc000, 7688, static_cast<std::uint8_t>(0xdd));
    Put(host, 0xa000, 0, 4);
    PutDescriptor(host, 0x7000, 0, 0xa000, 4, 1, 1);
    PutDescriptor(host, 0x7000, 1, 0xc000, 7688, 2, 0);
    PublishHead(host, 0x8000, 0, 1, 0);
    Assert(device->PumpCapture(1920) == 1920, "The post-reset RX queue completes its new descriptor", host);
    Assert(
        std::equal(expected.begin(), expected.end(), host.memory.begin() + 0xc000),
        "Reset invalidates the cached chain before new PCM writes",
        host);
    Assert(Get(host, 0xb800, 4) == 0xcccccccc, "Reset prevents writes through the pre-reset RX descriptor", host);
}

void ResetMsiProbe(FakeHost& host)
{
    host.msiCalls = 0;
    host.msiAddress = 0;
    host.msiData = 0;
    host.observeRxPublicationAtMsi = false;
    host.msiObservedExpectedRxPublication = false;
    host.expectedUsedIndexAddress = 0;
    host.expectedUsedEntryAddress = 0;
    host.expectedUsedIndex = 0;
    host.expectedUsedId = 0;
    host.expectedUsedLength = 0;
}

void ConfigureRxInterruptCaptureStream(FakeHost& host, VirtioSoundDevice& device, std::uint16_t rxVector = 2)
{
    Negotiate(device);
    ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(device, 3, 0x7000, 0x8000, 0x9000, rxVector);

    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1; // PCM_SET_PARAMS
    setParams[1] = 1;
    setParams[8] = 128; // 128-byte buffer
    setParams[12] = 64; // 16-frame period
    setParams[20] = 2;  // stereo
    setParams[21] = 5;  // S16_LE
    setParams[22] = 7;  // 48 kHz
    SubmitControl(host, device, 1, setParams.data(), setParams.size());

    std::array<std::uint8_t, 8> command{};
    command[0] = 2; // PREPARE
    command[1] = 1;
    SubmitControl(host, device, 2, command.data(), command.size());
    command[0] = 4; // START
    SubmitControl(host, device, 3, command.data(), command.size());
}

void ConfigureRxMsix(VirtioSoundDevice& device, bool functionMasked)
{
    BarWrite(device, 0x1020, 0xfee00000, 8); // vector 2 message address
    BarWrite(device, 0x1028, 0x55, 4);       // vector 2 message data
    BarWrite(device, 0x102c, 0, 4);          // vector 2 unmasked
    device.WriteConfig(0x86, functionMasked ? 0xc004 : 0x8004, 2);
}

void PublishRxDescriptor(FakeHost& host, std::uint16_t head, std::uint16_t ringSlot, std::uint16_t availableIndex, std::uint64_t requestAddress, std::uint64_t pcmAddress)
{
    Put(host, requestAddress, 0, 4); // stream 0
    PutDescriptor(host, 0x7000, head, requestAddress, 4, 1, static_cast<std::uint16_t>(head + 1));
    PutDescriptor(host, 0x7000, static_cast<std::uint16_t>(head + 1), pcmAddress, 72, 2, 0); // 16 frames + status
    PublishHead(host, 0x8000, ringSlot, availableIndex, head);
}

void ExpectRxPublicationAtNextMsi(FakeHost& host, std::uint16_t usedIndex, std::uint16_t usedSlot, std::uint32_t head, std::uint32_t usedLength)
{
    host.observeRxPublicationAtMsi = true;
    host.msiObservedExpectedRxPublication = false;
    host.expectedUsedIndexAddress = 0x9002;
    host.expectedUsedEntryAddress = 0x9004 + 8ULL * usedSlot;
    host.expectedUsedIndex = usedIndex;
    host.expectedUsedId = head;
    host.expectedUsedLength = usedLength;
}

void TestRxUsedPublicationPrecedesValidMsi(FakeHost& host)
{
    ClearGuestMemory(host);
    ResetMsiProbe(host);
    auto device = Create(host);
    ConfigureRxInterruptCaptureStream(host, *device);
    ConfigureRxMsix(*device, false);
    PublishRxDescriptor(host, 0, 0, 1, 0xa000, 0xb000);
    ExpectRxPublicationAtNextMsi(host, 1, 0, 0, 72);

    Assert(device->PumpCapture(16) == 16, "RX pump completes one available capture period", host);
    Assert(
        host.msiCalls == 1 && host.msiAddress == 0xfee00000 && host.msiData == 0x55,
        "RX completion delivers the configured valid MSI-X vector despite NO_NOTIFY",
        host);
    Assert(host.msiObservedExpectedRxPublication, "MSI callback observes the used entry and used index already published", host);
    Assert(
        Get(host, 0x9002, 2) == 1 && Get(host, 0x9004, 4) == 0 && Get(host, 0x9008, 4) == 72,
        "RX used ring retains the completed head and full length",
        host);
    Assert(Get(host, 0x9000, 2) == 1, "RX used flags retain NO_NOTIFY after a data completion", host);
}

void TestRxNoNotifyInitializationAndQueueIsolation(FakeHost& host)
{
    ClearGuestMemory(host);
    ResetMsiProbe(host);
    host.rxFlagsWriteObserved = false;
    host.rxFlagsWriteSawQueueDisabled = false;
    auto device = Create(host);

    // EVENT_IDX is not offered; only VERSION_1 is present in feature word 1.
    BarWrite(*device, 0, 0, 4);
    std::array<std::uint8_t, 4> featureWord{};
    device->ReadBar(4, featureWord.data(), featureWord.size());
    Assert(
        featureWord[0] == 0 && featureWord[1] == 0 && featureWord[2] == 0 && featureWord[3] == 0,
        "EVENT_IDX is not offered in device feature word 0",
        host);
    BarWrite(*device, 0, 1, 4);
    device->ReadBar(4, featureWord.data(), featureWord.size());
    Assert(
        featureWord[0] == 1 && featureWord[1] == 0 && featureWord[2] == 0 && featureWord[3] == 0,
        "VERSION_1 is the only offered feature in device feature word 1",
        host);

    // Modern virtio-pci drivers can configure queues after FEATURES_OK but before
    // DRIVER_OK. Writing the suppression hint must not consume a buffer or notify.
    BarWrite(*device, 8, 1, 4);  // driver feature page 1
    BarWrite(*device, 12, 1, 4); // accept VERSION_1 only
    BarWrite(*device, 20, 1, 1);
    BarWrite(*device, 20, 3, 1);
    BarWrite(*device, 20, 11, 1); // FEATURES_OK, not DRIVER_OK
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(*device, 1, 0x4000, 0x5000, 0x6000);
    ConfigureQueue(*device, 2, 0x7000, 0x8000, 0xa000);
    host.queueEnableProbeDevice = device.get();
    host.queueEnableProbeAddress = 0xd000;
    ConfigureQueue(*device, 3, 0xb000, 0xc000, 0xd000);
    host.queueEnableProbeDevice = nullptr;

    Assert(
        Get(host, 0x3000, 2) == 0 && Get(host, 0x6000, 2) == 0 && Get(host, 0xa000, 2) == 0,
        "Control, event, and TX used flags remain initialized to zero",
        host);
    Assert(Get(host, 0xd000, 2) == 1, "RX used flags contain VIRTQ_USED_F_NO_NOTIFY", host);
    Assert(
        host.rxFlagsWriteObserved && host.rxFlagsWriteSawQueueDisabled,
        "RX NO_NOTIFY is written before queue_enable becomes visible",
        host);
    Assert(
        Get(host, 0xd002, 2) == 0 && host.msiCalls == 0,
        "Pre-DRIVER_OK flag initialization neither consumes a buffer nor sends an interrupt",
        host);
    BarWrite(*device, 20, 15, 1);
    std::uint8_t status{};
    device->ReadBar(20, &status, 1);
    Assert(status == 15, "Device remains usable when DRIVER_OK follows RX flag initialization", host);

    device->Reset();
    ClearGuestMemory(host);
    Negotiate(*device);
    ConfigureQueue(*device, 3, 0xb000, 0xc000, 0xd000);
    Assert(Get(host, 0xd000, 2) == 1, "Reset followed by queue re-enable restores RX NO_NOTIFY", host);
}

void TestRxNoNotifyInitializationFailureLeavesQueueDisabled(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Negotiate(*device);

    BarWrite(*device, 22, 3, 2);
    BarWrite(*device, 32, 0x7000, 8);
    BarWrite(*device, 40, 0x8000, 8);
    BarWrite(*device, 48, 0x9000, 8);
    Put(host, 0x9000, 0xbeef, 2);
    host.failGuestWrite = true;
    host.failGuestWriteAddress = 0x9000;

    BarWrite(*device, 28, 1, 2);

    std::array<std::uint8_t, 2> queueEnabled{};
    device->ReadBar(28, queueEnabled.data(), queueEnabled.size());
    std::uint8_t status{};
    device->ReadBar(20, &status, 1);
    Assert(queueEnabled[0] == 0 && queueEnabled[1] == 0, "RX queue stays disabled when the used-flags guest write fails", host);
    Assert(Get(host, 0x9000, 2) == 0xbeef, "Failed RX used-flags write leaves guest memory unchanged", host);
    Assert((status & 64) != 0, "RX used-flags write failure faults the device", host);
    host.failGuestWrite = false;
}

void TestInvalidRxQueueDoesNotWriteGuestMemory(FakeHost& host)
{
    ClearGuestMemory(host);
    auto device = Create(host);
    Negotiate(*device);
    BarWrite(*device, 22, 3, 2);
    BarWrite(*device, 24, 3, 2); // invalid: queue size must be a power of two
    const int writesBeforeEnable = host.guestWriteCalls;
    BarWrite(*device, 28, 1, 2);

    std::array<std::uint8_t, 2> queueEnabled{};
    device->ReadBar(28, queueEnabled.data(), queueEnabled.size());
    Assert(host.guestWriteCalls == writesBeforeEnable, "Invalid RX queue configuration performs no guest-memory write", host);
    Assert(queueEnabled[0] == 0 && queueEnabled[1] == 0, "Invalid RX queue remains disabled", host);
}

void TestRxMaskedPbaUnmasksExactlyOnce(FakeHost& host)
{
    ClearGuestMemory(host);
    ResetMsiProbe(host);
    auto device = Create(host);
    ConfigureRxInterruptCaptureStream(host, *device);
    ConfigureRxMsix(*device, true);
    PublishRxDescriptor(host, 0, 0, 1, 0xa000, 0xb000);
    ExpectRxPublicationAtNextMsi(host, 1, 0, 0, 72);

    Assert(device->PumpCapture(16) == 16, "Function-masked RX vector still permits capture completion", host);
    std::array<std::uint8_t, 8> pba{};
    device->ReadBar(0x1800, pba.data(), pba.size());
    const auto pbaValue = [&pba] {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < pba.size(); ++i)
        {
            value |= static_cast<std::uint64_t>(pba[i]) << (8 * i);
        }
        return value;
    };
    Assert(
        host.msiCalls == 0 && Get(host, 0x9002, 2) == 1 && Get(host, 0x9008, 4) == 72,
        "Masked RX completion publishes its used ring before interrupt delivery",
        host);
    Assert(pbaValue() == (1ULL << 2), "Function-masked RX vector is recorded in the PBA", host);

    device->WriteConfig(0x86, 0x8004, 2); // unmask MSI-X function
    Assert(
        host.msiCalls == 1 && host.msiAddress == 0xfee00000 && host.msiData == 0x55, "Unmask delivers the pending RX vector once", host);
    Assert(host.msiObservedExpectedRxPublication, "Unmasked MSI callback sees the completed RX used ring", host);
    device->WriteConfig(0x86, 0x8004, 2);
    device->ReadBar(0x1800, pba.data(), pba.size());
    Assert(host.msiCalls == 1 && pbaValue() == 0, "Repeated unmask does not redeliver the cleared pending RX vector", host);
}

void TestRxNoInterruptSuppressesOnlyFlaggedCompletion(FakeHost& host)
{
    ClearGuestMemory(host);
    ResetMsiProbe(host);
    auto device = Create(host);
    ConfigureRxInterruptCaptureStream(host, *device);
    ConfigureRxMsix(*device, false);

    Put(host, 0x8000, 1, 2); // VRING_AVAIL_F_NO_INTERRUPT for first completion
    PublishRxDescriptor(host, 0, 0, 1, 0xa000, 0xb000);
    Assert(device->PumpCapture(16) == 16, "RX capture completes with notification suppression set", host);
    Assert(
        host.msiCalls == 0 && Get(host, 0x9002, 2) == 1, "NO_INTERRUPT suppresses MSI without suppressing used-ring publication", host);
    Assert(Get(host, 0x9004, 4) == 0 && Get(host, 0x9008, 4) == 72, "Suppressed RX completion publishes its used entry", host);

    Put(host, 0x8000, 0, 2); // re-enable notifications for the next completion
    PublishRxDescriptor(host, 2, 1, 2, 0xa100, 0xb100);
    ExpectRxPublicationAtNextMsi(host, 2, 1, 2, 72);
    Assert(device->PumpCapture(16) == 16, "Second RX capture completes after notification is re-enabled", host);
    Assert(
        host.msiCalls == 1 && host.msiObservedExpectedRxPublication,
        "The following completion sends one MSI only after publishing its used entry/index",
        host);
    Assert(
        Get(host, 0x9002, 2) == 2 && Get(host, 0x900c, 4) == 2 && Get(host, 0x9010, 4) == 72,
        "Second RX head occupies the next used-ring slot",
        host);
}

void TestRxQueueNotifyDoesNotConsumeBeforePump(FakeHost& host)
{
    ClearGuestMemory(host);
    ResetMsiProbe(host);
    auto source = std::make_shared<CountingPcmSource>();
    GuestMemoryCallbacks memory{&host, &Read, &Write};
    InterruptCallbacks interrupts{&host, &Msi, nullptr};
    auto device = std::make_unique<VirtioSoundDevice>(memory, interrupts, source, PcmFormat{});
    ConfigureRxInterruptCaptureStream(host, *device);
    PublishRxDescriptor(host, 0, 0, 1, 0xa000, 0xb000);

    const DiagnosticSnapshot beforeNotify = device->GetDiagnosticsSnapshot();
    BarWrite(*device, 0x30c, 3, 2); // queue 3 notification
    const DiagnosticSnapshot afterNotify = device->GetDiagnosticsSnapshot();
    Assert(
        source->readCalls == 0 && Get(host, 0x9002, 2) == 0, "RX queue notification does not read PCM or consume an RX descriptor", host);
    Assert(
        afterNotify.m_rxAvailableProcessed == beforeNotify.m_rxAvailableProcessed &&
            afterNotify.m_rxUsedEntriesPublished == beforeNotify.m_rxUsedEntriesPublished,
        "RX queue notification leaves receive progress counters unchanged",
        host);

    Assert(device->PumpCapture(16) == 16, "Explicit capture pump consumes the queued RX descriptor", host);
    const DiagnosticSnapshot afterPump = device->GetDiagnosticsSnapshot();
    Assert(
        source->readCalls == 1 && Get(host, 0x9002, 2) == 1 && afterPump.m_rxUsedEntriesPublished == 1,
        "The pump, rather than the RX notification, performs capture and used publication",
        host);
}

void TestSourceXrunAndReleaseDrain(FakeHost& host)
{
    ClearGuestMemory(host);
    auto failingSource = std::make_shared<FailingPcmSource>();
    GuestMemoryCallbacks memory{&host, &Read, &Write};
    InterruptCallbacks interrupts{&host, &Msi, nullptr};
    auto device = std::make_unique<VirtioSoundDevice>(memory, interrupts, failingSource, PcmFormat{});
    Negotiate(*device);
    ConfigureQueue(*device, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(*device, 1, 0xd000, 0xe000, 0xf000);
    ConfigureQueue(*device, 3, 0x7000, 0x8000, 0x9000);

    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1;
    setParams[1] = 1; // PCM_SET_PARAMS
    setParams[8] = 64;
    setParams[12] = 32;
    setParams[16] = 16; // negotiate EVT_XRUNS
    setParams[20] = 2;
    setParams[21] = 5;
    setParams[22] = 7;
    SubmitControl(host, *device, 1, setParams.data(), setParams.size());
    std::array<std::uint8_t, 8> command{};
    command[0] = 2;
    command[1] = 1; // PREPARE
    SubmitControl(host, *device, 2, command.data(), command.size());
    command[0] = 4; // START
    SubmitControl(host, *device, 3, command.data(), command.size());

    std::fill_n(host.memory.begin() + 0xb000, 16, static_cast<std::uint8_t>(0xcc));
    Put(host, 0xa000, 0, 4);
    PutDescriptor(host, 0x7000, 0, 0xa000, 4, 1, 1);
    PutDescriptor(host, 0x7000, 1, 0xb000, 16, 2, 0);
    PublishHead(host, 0x8000, 0, 1, 0);
    PutDescriptor(host, 0xd000, 0, 0x6100, 8, 2, 0); // writable event buffer
    PublishHead(host, 0xe000, 0, 1, 0);
    Assert(device->PumpCapture(2) == 0 && failingSource->readCalls == 1, "PCM source failure reports no data frames", host);
    Assert(
        Get(host, 0xb008, 4) == 0x8003 && Get(host, 0x9008, 4) == 8,
        "PCM source failure publishes an IO_ERR status-only RX completion",
        host);
    Assert(Get(host, 0xb000, 4) == 0xcccccccc, "PCM source failure leaves guest PCM data untouched", host);
    Assert(
        Get(host, 0x6100, 4) == 0x1101 && Get(host, 0x6104, 4) == 0,
        "Negotiated XRUN feature publishes the stream-0 PCM_XRUN event",
        host);
    Assert(Get(host, 0xf002, 2) == 1, "XRUN event buffer is completed on the event queue", host);

    ClearGuestMemory(host);
    auto releaseDevice = Create(host);
    Negotiate(*releaseDevice);
    ConfigureQueue(*releaseDevice, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(*releaseDevice, 3, 0x7000, 0x8000, 0x9000);
    setParams[16] = 0;
    SubmitControl(host, *releaseDevice, 1, setParams.data(), setParams.size());
    command[0] = 2; // PREPARE; RELEASE is valid from this state
    SubmitControl(host, *releaseDevice, 2, command.data(), command.size());
    Put(host, 0xa000, 0, 4);
    PutDescriptor(host, 0x7000, 0, 0xa000, 4, 1, 1);
    PutDescriptor(host, 0x7000, 1, 0xb000, 16, 2, 0);
    PublishHead(host, 0x8000, 0, 1, 0);
    command[0] = 3; // RELEASE
    SubmitControl(host, *releaseDevice, 3, command.data(), command.size());
    Assert(
        Get(host, 0xb008, 4) == 0x8003 && Get(host, 0x9002, 2) == 1 && Get(host, 0x9008, 4) == 8,
        "Successful RELEASE drains outstanding RX buffers with IO_ERR before replying",
        host);
    Assert(Get(host, 0x5000, 4) == 0x8000 && Get(host, 0x3002, 2) == 3, "Control RELEASE response completes after the RX drain", host);
}

} // namespace

int main()
{
    try
    {
        FakeHost host;
        TestPciAndControl(host);
        TestMalformedDescriptorsAndReset(host);
        TestAddressOverflowAndMsix(host);
        TestFormatAndSyntheticCapture(host);
        TestPendingRxChainCacheAndReset(host);
        TestRxNoNotifyInitializationAndQueueIsolation(host);
        TestRxNoNotifyInitializationFailureLeavesQueueDisabled(host);
        TestInvalidRxQueueDoesNotWriteGuestMemory(host);
        TestRxUsedPublicationPrecedesValidMsi(host);
        TestRxMaskedPbaUnmasksExactlyOnce(host);
        TestRxNoInterruptSuppressesOnlyFlaggedCompletion(host);
        TestRxQueueNotifyDoesNotConsumeBeforePump(host);
        TestSourceXrunAndReleaseDrain(host);
        std::cout << "PASS: native virtio-sound core "
                  << host.assertions << " assertions; malformed control/descriptor, GPA overflow, RX notification suppression, MSI-X, and reset paths.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
