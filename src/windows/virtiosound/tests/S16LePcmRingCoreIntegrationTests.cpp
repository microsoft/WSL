// Copyright (C) Microsoft Corporation. All rights reserved.

#include "S16LePcmRing.hpp"
#include "VirtioSoundDevice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using wsl::virtio_sound::native::DiagnosticSnapshot;
using wsl::virtio_sound::native::GuestMemoryCallbacks;
using wsl::virtio_sound::native::InterruptCallbacks;
using wsl::virtio_sound::native::PcmFormat;
using wsl::virtio_sound::native::PcmRingWriteResult;
using wsl::virtio_sound::native::S16LePcmRingSource;
using wsl::virtio_sound::native::VirtioSoundDevice;

namespace {
struct FakeGuest
{
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(4 * 1024 * 1024);
};

void Assert(bool condition, const char* message, int& assertions)
{
    ++assertions;
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
    if (count != 0)
    {
        std::copy_n(guest.memory.data() + static_cast<std::size_t>(address), count, destination);
    }
    return true;
}

bool WriteGuest(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t count)
{
    auto& guest = *static_cast<FakeGuest*>(context);
    if (address > guest.memory.size() || count > guest.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0)
    {
        std::copy_n(source, count, guest.memory.data() + static_cast<std::size_t>(address));
    }
    return true;
}

bool DeliverMsi(void*, std::uint64_t, std::uint32_t)
{
    return true;
}

void Put(FakeGuest& guest, std::uint64_t address, std::uint64_t value, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i)
    {
        guest.memory[static_cast<std::size_t>(address) + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::uint64_t Get(const FakeGuest& guest, std::uint64_t address, std::size_t length)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < length; ++i)
    {
        value |= static_cast<std::uint64_t>(guest.memory[static_cast<std::size_t>(address) + i]) << (8 * i);
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

void ConfigureQueue(VirtioSoundDevice& device, std::uint16_t index, std::uint64_t descriptor, std::uint64_t available, std::uint64_t used)
{
    BarWrite(device, 22, index, 2);
    BarWrite(device, 26, 0xffff, 2);
    BarWrite(device, 32, descriptor, 8);
    BarWrite(device, 40, available, 8);
    BarWrite(device, 48, used, 8);
    BarWrite(device, 28, 1, 2);
}

void Negotiate(VirtioSoundDevice& device)
{
    BarWrite(device, 0, 1, 4);
    BarWrite(device, 8, 1, 4);
    BarWrite(device, 12, 1, 4);
    BarWrite(device, 20, 1, 1);
    BarWrite(device, 20, 3, 1);
    BarWrite(device, 20, 11, 1);
    BarWrite(device, 20, 15, 1);
}

void PutDescriptor(FakeGuest& guest, std::uint64_t table, std::uint16_t index, std::uint64_t address, std::uint32_t length, std::uint16_t flags, std::uint16_t next)
{
    const std::uint64_t at = table + 16ULL * index;
    Put(guest, at, address, 8);
    Put(guest, at + 8, length, 4);
    Put(guest, at + 12, flags, 2);
    Put(guest, at + 14, next, 2);
}

void PublishHead(FakeGuest& guest, std::uint64_t available, std::uint16_t slot, std::uint16_t availableIndex, std::uint16_t head)
{
    Put(guest, available + 4 + 2ULL * slot, head, 2);
    Put(guest, available + 2, availableIndex, 2);
}

void SubmitControl(FakeGuest& guest, VirtioSoundDevice& device, std::uint16_t availableIndex, const std::uint8_t* request, std::size_t requestLength, int& assertions)
{
    std::copy_n(request, requestLength, guest.memory.begin() + 0x4000);
    PutDescriptor(guest, 0x1000, 0, 0x4000, static_cast<std::uint32_t>(requestLength), 1, 1);
    PutDescriptor(guest, 0x1000, 1, 0x5000, 4, 2, 0);
    PublishHead(guest, 0x2000, static_cast<std::uint16_t>(availableIndex - 1), availableIndex, 0);
    Assert(device.ProcessControlQueue() == 1, "Virtio PCM control command completes", assertions);
    Assert(Get(guest, 0x5000, 4) == 0x8000, "Virtio PCM control command returns OK", assertions);
}

void SetUpDevice(FakeGuest& guest, VirtioSoundDevice& device, std::uint32_t bufferBytes, std::uint32_t periodBytes, int& assertions)
{
    Negotiate(device);
    ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(device, 3, 0x10000, 0x12000, 0x13000);

    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1;
    setParams[1] = 1; // PCM_SET_PARAMS (0x0101)
    for (std::size_t i = 0; i < 4; ++i)
    {
        setParams[8 + i] = static_cast<std::uint8_t>(bufferBytes >> (8 * i));
        setParams[12 + i] = static_cast<std::uint8_t>(periodBytes >> (8 * i));
    }
    setParams[20] = 2; // stereo
    setParams[21] = 5; // S16_LE
    setParams[22] = 7; // 48 kHz
    SubmitControl(guest, device, 1, setParams.data(), setParams.size(), assertions);

    std::array<std::uint8_t, 8> command{};
    command[0] = 2;
    command[1] = 1; // PCM_PREPARE
    SubmitControl(guest, device, 2, command.data(), command.size(), assertions);
    command[0] = 4; // PCM_START
    SubmitControl(guest, device, 3, command.data(), command.size(), assertions);
}

void EncodeStereoRamp(std::uint32_t sequence, std::uint8_t* destination)
{
    const std::uint16_t left = static_cast<std::uint16_t>(sequence);
    const std::uint16_t right = static_cast<std::uint16_t>(sequence ^ 0xa55aU);
    destination[0] = static_cast<std::uint8_t>(left);
    destination[1] = static_cast<std::uint8_t>(left >> 8);
    destination[2] = static_cast<std::uint8_t>(right);
    destination[3] = static_cast<std::uint8_t>(right >> 8);
}

void FillStereoFrames(std::uint32_t first, std::uint32_t count, std::uint8_t* destination)
{
    for (std::uint32_t frame = 0; frame < count; ++frame)
    {
        EncodeStereoRamp(first + frame, destination + static_cast<std::size_t>(frame) * 4);
    }
}

void QueueCaptureDescriptors(FakeGuest& guest, std::uint16_t count, std::uint32_t payloadBytes)
{
    const std::uint32_t stride = payloadBytes + 8;
    for (std::uint16_t i = 0; i < count; ++i)
    {
        const std::uint16_t requestIndex = static_cast<std::uint16_t>(i * 2);
        const std::uint64_t requestAddress = 0x20000 + static_cast<std::uint64_t>(i) * 4;
        const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(i) * stride;
        Put(guest, requestAddress, 0, 4); // capture stream ID
        PutDescriptor(guest, 0x10000, requestIndex, requestAddress, 4, 1, static_cast<std::uint16_t>(requestIndex + 1));
        PutDescriptor(guest, 0x10000, static_cast<std::uint16_t>(requestIndex + 1), payloadAddress, stride, 2, 0);
        PublishHead(guest, 0x12000, i, static_cast<std::uint16_t>(i + 1), requestIndex);
    }
}

std::unique_ptr<VirtioSoundDevice> CreateDevice(FakeGuest& guest, const std::shared_ptr<S16LePcmRingSource>& ring)
{
    GuestMemoryCallbacks memory{&guest, &ReadGuest, &WriteGuest};
    InterruptCallbacks interrupts{&guest, &DeliverMsi, nullptr};
    return std::make_unique<VirtioSoundDevice>(memory, interrupts, ring, PcmFormat{2, 48000});
}

void TestConcurrentProducerAndCorePump(int& assertions)
{
    constexpr std::uint32_t c_periodFrames = 4096;
    constexpr std::uint16_t c_queueEntries = 64;
    constexpr std::uint32_t c_totalFrames = c_periodFrames * c_queueEntries;
    constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
    constexpr std::uint32_t c_totalBytes = c_totalFrames * 4;
    constexpr std::size_t c_ringCapacityFrames = static_cast<std::size_t>(c_totalFrames) * 2;

    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, 48000}, c_ringCapacityFrames);
    auto device = CreateDevice(guest, ring);
    SetUpDevice(guest, *device, static_cast<std::uint32_t>(c_ringCapacityFrames * 4), c_periodBytes, assertions);

    const std::uint64_t generation = ring->Generation();
    std::vector<std::uint8_t> initial(c_totalBytes);
    std::vector<std::uint8_t> incoming(c_totalBytes);
    FillStereoFrames(0, c_totalFrames, initial.data());
    FillStereoFrames(c_totalFrames, c_totalFrames, incoming.data());
    Assert(
        ring->TryWriteFrames(generation, initial.data(), c_totalFrames, initial.size()) == PcmRingWriteResult::Written,
        "Core RX periods are fully prebuffered before capture",
        assertions);

    QueueCaptureDescriptors(guest, c_queueEntries, c_periodBytes);
    std::atomic<bool> producerReady{};
    std::atomic<bool> begin{};
    std::atomic<PcmRingWriteResult> producerResult{PcmRingWriteResult::EmptyWrite};
    std::thread producer([&] {
        producerReady.store(true, std::memory_order_release);
        while (!begin.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        producerResult.store(ring->TryWriteFrames(generation, incoming.data(), c_totalFrames, incoming.size()), std::memory_order_release);
    });
    while (!producerReady.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    begin.store(true, std::memory_order_release);
    const int completedFrames = device->PumpCapture(static_cast<int>(c_totalFrames));
    producer.join();

    Assert(
        producerResult.load(std::memory_order_acquire) == PcmRingWriteResult::Written,
        "Concurrent producer publishes its batch while core capture pumps",
        assertions);
    Assert(
        completedFrames == static_cast<int>(c_totalFrames),
        "Core capture completes all prebuffered frames during producer activity",
        assertions);
    Assert(
        Get(guest, 0x12000 + 2, 2) == c_queueEntries && Get(guest, 0x13000 + 2, 2) == c_queueEntries,
        "All RX descriptors publish used entries",
        assertions);

    const std::uint32_t stride = c_periodBytes + 8;
    for (std::uint16_t i = 0; i < c_queueEntries; ++i)
    {
        const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(i) * stride;
        const std::uint32_t expectedSequence = static_cast<std::uint32_t>(i) * c_periodFrames;
        std::array<std::uint8_t, 8> expected{};
        for (std::uint32_t frame = 0; frame < 2; ++frame)
        {
            EncodeStereoRamp(expectedSequence + frame, expected.data() + static_cast<std::size_t>(frame) * 4);
        }
        Assert(
            std::equal(expected.begin(), expected.end(), guest.memory.begin() + static_cast<std::ptrdiff_t>(payloadAddress)),
            "Core delivers the oldest interleaved producer frames in order",
            assertions);
        Assert(
            Get(guest, payloadAddress + c_periodBytes, 4) == 0x8000,
            "Producer overlap does not turn available capture into IO_ERR/XRUN",
            assertions);
    }

    const DiagnosticSnapshot diagnostics = device->GetDiagnosticsSnapshot();
    Assert(
        diagnostics.m_rxDataCompletions == c_queueEntries && diagnostics.m_rxStatusOnlyCompletions == 0,
        "Capture pump reports data completions and no status-only XRUNs",
        assertions);
    const auto ringStats = ring->Snapshot();
    Assert(
        ringStats.m_underflowEvents == 0 && ringStats.m_sequenceFailures == 0,
        "Sufficient prebuffer distinguishes concurrency from genuine underflow or index faults",
        assertions);
}

void TestCoreUnderflowRemainsAnError(int& assertions)
{
    constexpr std::uint32_t c_periodFrames = 256;
    constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, 48000}, 1024);
    auto device = CreateDevice(guest, ring);
    SetUpDevice(guest, *device, 4096, c_periodBytes, assertions);
    QueueCaptureDescriptors(guest, 1, c_periodBytes);

    Assert(device->PumpCapture(static_cast<int>(c_periodFrames)) == 0, "A genuine empty-ring underflow produces no data frames", assertions);
    Assert(
        Get(guest, 0x30000 + c_periodBytes, 4) == 0x8003 && Get(guest, 0x13000 + 8, 4) == 8,
        "Genuine source underflow remains an IO_ERR status-only completion",
        assertions);
    const DiagnosticSnapshot diagnostics = device->GetDiagnosticsSnapshot();
    Assert(
        diagnostics.m_rxStatusOnlyCompletions == 1 && diagnostics.m_rxDataCompletions == 0 && ring->Snapshot().m_underflowFramesMissing == c_periodFrames,
        "Underflow counters identify missing frames separately from producer contention",
        assertions);
}

} // namespace

int main()
{
    try
    {
        int assertions = 0;
        TestConcurrentProducerAndCorePump(assertions);
        TestCoreUnderflowRemainsAnError(assertions);
        std::cout << "PASS: ring/core integration " << assertions
                  << " assertions; concurrent prebuffered capture has no false XRUN, true underflow remains IO_ERR.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
