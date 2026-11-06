// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HdvPciDeviceHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wsl::virtio_sound::native::windows::testing {
void InstallHdvApiForTesting(
    decltype(&::HdvInitializeDeviceHost) initialize,
    decltype(&::HdvTeardownDeviceHost) teardown,
    decltype(&::HdvCreateDeviceInstance) createDevice,
    decltype(&::HdvReadGuestMemory) readGuestMemory,
    decltype(&::HdvWriteGuestMemory) writeGuestMemory,
    decltype(&::HdvDeliverGuestInterrupt) deliverGuestInterrupt);
void InstallGuestMemoryApertureApiForTesting(decltype(&::HdvCreateGuestMemoryAperture) createAperture, decltype(&::HdvDestroyGuestMemoryAperture) destroyAperture);
void SetLifecycleTestHook(void (*hook)(void*, unsigned int) noexcept, void* context);
using AperturePumpTestHook = void (*)(void* backendContext, void* hookContext) noexcept;
using ApertureCopyTestHook = void (*)(void* hookContext, bool write) noexcept;
using ApertureBatchEndTestHook = void (*)(void* hookContext) noexcept;
void SetAperturePumpTestHook(AperturePumpTestHook hook, void* context);
void SetApertureCopyTestHook(ApertureCopyTestHook hook, void* context);
void SetApertureBatchEndTestHook(ApertureBatchEndTestHook hook, void* context);
bool InvokeGuestMemoryCallbackForTesting(void* backendContext, bool write, UINT64 address, BYTE* buffer, UINT32 byteCount) noexcept;
struct GuestMemoryApertureBatchStats final
{
    std::uint64_t mappedReadCopies{};
    std::uint64_t mappedWriteCopies{};
    std::uint64_t fallbackReadCalls{};
    std::uint64_t fallbackWriteCalls{};
    std::uint64_t createCalls{};
    std::uint64_t createFailures{};
    std::uint64_t destroyCalls{};
    std::uint64_t destroyFailures{};
    std::uint64_t peakEntries{};
    std::uint64_t createElapsedNanoseconds{};
    std::uint64_t destroyElapsedNanoseconds{};
};
GuestMemoryApertureBatchStats GetGuestMemoryApertureBatchStatsForTesting() noexcept;
void ResetGuestMemoryApertureBatchStatsForTesting() noexcept;
} // namespace wsl::virtio_sound::native::windows::testing

namespace {
using namespace wsl::virtio_sound::native::windows;

constexpr unsigned int c_testHookCallbackAdmitted = 1;
constexpr unsigned int c_testHookRetireDisarmed = 2;

constexpr GUID c_deviceClassId{0xF1919B41, 0x6A33, 0x4C92, {0xA7, 0xF8, 0x39, 0x7E, 0xE4, 0xC5, 0xB1, 0xD2}};
constexpr GUID c_deviceInstanceId{0x2F6C310A, 0x51D2, 0x4EC0, {0x9D, 0x81, 0x48, 0x9A, 0x3C, 0xC2, 0x77, 0x61}};
constexpr std::uintptr_t c_fakeSystemValue = 0x12340;
constexpr std::uintptr_t c_fakeHostValue = 0x23450;
constexpr std::uintptr_t c_fakeDeviceValue = 0x34560;
constexpr UINT64 c_fakeGuestMemoryBase = 0x100000;
constexpr std::size_t c_fakeGuestMemoryBytes = 1024 * 1024;

struct FakeGuestAperture final
{
    UINT64 guestPhysicalAddress{};
    UINT32 byteCount{};
    BOOL writeProtected{};
    PVOID mappedAddress{};
};

struct FakeHdv final
{
    std::mutex mutex;
    std::condition_variable apertureChanged;
    HCS_SYSTEM expectedSystem{};
    HDV_PCI_DEVICE_INTERFACE deviceInterface{};
    PVOID callbackContext{};
    GUID classId{};
    GUID instanceId{};
    HDV_HOST lastHost{};
    HDV_DEVICE lastDevice{};
    HRESULT initializeResult{S_OK};
    HRESULT createResult{S_OK};
    HRESULT teardownResult{S_OK};
    HRESULT createApertureResult{S_OK};
    HRESULT destroyApertureResult{S_OK};
    bool returnNullDevice{};
    bool returnNullAperture{};
    bool returnApertureOnCreateFailure{};
    bool returnInvalidApertureAddress{};
    bool destroyAttemptObserved{};
    unsigned int initializeCalls{};
    unsigned int createCalls{};
    unsigned int teardownCalls{};
    unsigned int readGuestMemoryCalls{};
    unsigned int writeGuestMemoryCalls{};
    unsigned int createApertureCalls{};
    unsigned int destroyApertureCalls{};
    bool createArgumentsValid{};
    bool initializeSystemValid{};
    std::array<BYTE, c_fakeGuestMemoryBytes> guestMemory{};
    std::vector<FakeGuestAperture> activeApertures;
    std::vector<FakeGuestAperture> apertureCreateLog;
};

FakeHdv g_fake;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

HCS_SYSTEM FakeSystem() noexcept
{
    return reinterpret_cast<HCS_SYSTEM>(c_fakeSystemValue);
}

HDV_HOST FakeHost() noexcept
{
    return reinterpret_cast<HDV_HOST>(c_fakeHostValue);
}

HDV_DEVICE FakeDevice() noexcept
{
    return reinterpret_cast<HDV_DEVICE>(c_fakeDeviceValue);
}

bool EqualGuid(const GUID& left, const GUID& right) noexcept
{
    return left.Data1 == right.Data1 && left.Data2 == right.Data2 && left.Data3 == right.Data3 &&
           std::equal(std::begin(left.Data4), std::end(left.Data4), std::begin(right.Data4));
}

HRESULT WINAPI FakeInitialize(HCS_SYSTEM system, HDV_HOST* deviceHost)
{
    if (deviceHost == nullptr)
    {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.initializeCalls;
    g_fake.initializeSystemValid = system == g_fake.expectedSystem;
    g_fake.lastHost = FakeHost();
    *deviceHost = g_fake.lastHost;
    return g_fake.initializeResult;
}

HRESULT WINAPI FakeTeardown(HDV_HOST deviceHost)
{
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.teardownCalls;
    if (deviceHost != g_fake.lastHost)
    {
        return E_HANDLE;
    }
    return g_fake.teardownResult;
}

HRESULT WINAPI FakeCreateDevice(
    HDV_HOST deviceHost,
    HDV_DEVICE_TYPE deviceType,
    const GUID* deviceClassId,
    const GUID* deviceInstanceId,
    const VOID* deviceInterface,
    VOID* deviceContext,
    HDV_DEVICE* deviceHandle)
{
    if (deviceHandle == nullptr)
    {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.createCalls;
    g_fake.createArgumentsValid = deviceHost == g_fake.lastHost && deviceType == HdvDeviceTypePCI && deviceClassId != nullptr &&
                                  deviceInstanceId != nullptr && deviceInterface != nullptr && deviceContext != nullptr &&
                                  EqualGuid(*deviceClassId, c_deviceClassId) && EqualGuid(*deviceInstanceId, c_deviceInstanceId);
    if (deviceInterface != nullptr)
    {
        g_fake.deviceInterface = *static_cast<const HDV_PCI_DEVICE_INTERFACE*>(deviceInterface);
        g_fake.callbackContext = deviceContext;
    }
    *deviceHandle = g_fake.returnNullDevice ? nullptr : FakeDevice();
    g_fake.lastDevice = *deviceHandle;
    return g_fake.createResult;
}

bool FakeGuestMemoryOffset(UINT64 address, UINT32 byteCount, std::size_t& offset) noexcept
{
    if (address < c_fakeGuestMemoryBase)
    {
        return false;
    }
    const UINT64 offset64 = address - c_fakeGuestMemoryBase;
    if (offset64 > c_fakeGuestMemoryBytes || byteCount > c_fakeGuestMemoryBytes - static_cast<std::size_t>(offset64))
    {
        return false;
    }
    offset = static_cast<std::size_t>(offset64);
    return true;
}

HRESULT WINAPI FakeReadGuestMemory(HDV_DEVICE, UINT64 address, UINT32 byteCount, BYTE* buffer)
{
    if (byteCount != 0 && buffer == nullptr)
    {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.readGuestMemoryCalls;
    std::size_t offset{};
    if (!FakeGuestMemoryOffset(address, byteCount, offset))
    {
        return E_INVALIDARG;
    }
    if (byteCount != 0)
    {
        std::memcpy(buffer, g_fake.guestMemory.data() + offset, byteCount);
    }
    return S_OK;
}

HRESULT WINAPI FakeWriteGuestMemory(HDV_DEVICE, UINT64 address, UINT32 byteCount, const BYTE* buffer)
{
    if (byteCount != 0 && buffer == nullptr)
    {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.writeGuestMemoryCalls;
    std::size_t offset{};
    if (!FakeGuestMemoryOffset(address, byteCount, offset))
    {
        return E_INVALIDARG;
    }
    if (byteCount != 0)
    {
        std::memcpy(g_fake.guestMemory.data() + offset, buffer, byteCount);
    }
    return S_OK;
}

HRESULT WINAPI FakeCreateGuestMemoryAperture(HDV_DEVICE requestor, UINT64 guestPhysicalAddress, UINT32 byteCount, BOOL writeProtected, PVOID* mappedAddress)
{
    if (mappedAddress == nullptr)
    {
        return E_POINTER;
    }
    *mappedAddress = nullptr;
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.createApertureCalls;
    if (requestor != g_fake.lastDevice)
    {
        return E_HANDLE;
    }
    if (byteCount == 0 || (guestPhysicalAddress % 4096) != 0 || (byteCount % 4096) != 0)
    {
        return E_INVALIDARG;
    }
    std::size_t offset{};
    if (!FakeGuestMemoryOffset(guestPhysicalAddress, byteCount, offset))
    {
        return E_INVALIDARG;
    }
    if (SUCCEEDED(g_fake.createApertureResult) && g_fake.returnNullAperture)
    {
        return S_OK;
    }
    if (FAILED(g_fake.createApertureResult) && !g_fake.returnApertureOnCreateFailure)
    {
        return g_fake.createApertureResult;
    }
    *mappedAddress = g_fake.returnInvalidApertureAddress ? reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(1))
                                                         : g_fake.guestMemory.data() + offset;
    const FakeGuestAperture aperture{guestPhysicalAddress, byteCount, writeProtected, *mappedAddress};
    g_fake.activeApertures.push_back(aperture);
    g_fake.apertureCreateLog.push_back(aperture);
    return g_fake.createApertureResult;
}

HRESULT WINAPI FakeDestroyGuestMemoryAperture(HDV_DEVICE requestor, PVOID mappedAddress)
{
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    ++g_fake.destroyApertureCalls;
    g_fake.destroyAttemptObserved = true;
    g_fake.apertureChanged.notify_all();
    if (requestor != g_fake.lastDevice)
    {
        return E_HANDLE;
    }
    auto aperture = std::find_if(g_fake.activeApertures.begin(), g_fake.activeApertures.end(), [mappedAddress](const auto& value) {
        return value.mappedAddress == mappedAddress;
    });
    if (aperture == g_fake.activeApertures.end())
    {
        return E_HANDLE;
    }
    if (FAILED(g_fake.destroyApertureResult))
    {
        return g_fake.destroyApertureResult;
    }
    g_fake.activeApertures.erase(aperture);
    return S_OK;
}

HRESULT WINAPI FakeDeliverGuestInterrupt(HDV_DEVICE, UINT64, UINT32)
{
    return S_OK;
}

void ResetFake(HRESULT createResult = S_OK, bool returnNullDevice = false, HRESULT teardownResult = S_OK)
{
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    g_fake.expectedSystem = FakeSystem();
    g_fake.deviceInterface = {};
    g_fake.callbackContext = nullptr;
    g_fake.classId = {};
    g_fake.instanceId = {};
    g_fake.lastHost = nullptr;
    g_fake.lastDevice = nullptr;
    g_fake.initializeResult = S_OK;
    g_fake.createResult = createResult;
    g_fake.teardownResult = teardownResult;
    g_fake.createApertureResult = S_OK;
    g_fake.destroyApertureResult = S_OK;
    g_fake.returnNullDevice = returnNullDevice;
    g_fake.returnNullAperture = false;
    g_fake.returnApertureOnCreateFailure = false;
    g_fake.returnInvalidApertureAddress = false;
    g_fake.destroyAttemptObserved = false;
    g_fake.initializeCalls = 0;
    g_fake.createCalls = 0;
    g_fake.teardownCalls = 0;
    g_fake.readGuestMemoryCalls = 0;
    g_fake.writeGuestMemoryCalls = 0;
    g_fake.createApertureCalls = 0;
    g_fake.destroyApertureCalls = 0;
    g_fake.createArgumentsValid = false;
    g_fake.initializeSystemValid = false;
    g_fake.guestMemory.fill(0);
    g_fake.activeApertures.clear();
    g_fake.apertureCreateLog.clear();
}

struct CallbackSnapshot final
{
    HDV_PCI_DEVICE_INTERFACE deviceInterface{};
    PVOID context{};
};

CallbackSnapshot GetCallbackSnapshot()
{
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    return {g_fake.deviceInterface, g_fake.callbackContext};
}

void SetFakeGuestMemory(UINT64 address, const BYTE* bytes, UINT32 byteCount)
{
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    std::size_t offset{};
    Require(FakeGuestMemoryOffset(address, byteCount, offset), "test guest-memory initialization range was invalid");
    if (byteCount != 0)
    {
        std::memcpy(g_fake.guestMemory.data() + offset, bytes, byteCount);
    }
}

struct PumpApertureProbe final
{
    void* callbackContext{};
    UINT64 address{};
    std::array<BYTE, 16> initial{};
    std::array<BYTE, 16> writeOne{};
    std::array<BYTE, 16> writeTwo{};
    std::array<BYTE, 16> firstRead{};
    std::array<BYTE, 16> finalRead{};
    unsigned int pumpCalls{};
    bool callbacksSucceeded{true};
    bool contentsMatched{true};
};

void ExerciseGuestMemoryDuringPump(void* backendContext, void* hookContext) noexcept
{
    auto& probe = *static_cast<PumpApertureProbe*>(hookContext);
    probe.callbackContext = backendContext;
    std::array<BYTE, 16> readBefore{};
    if (!testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, false, probe.address, readBefore.data(), static_cast<UINT32>(readBefore.size())))
    {
        probe.callbacksSucceeded = false;
    }
    if (probe.pumpCalls == 0)
    {
        probe.contentsMatched = probe.contentsMatched && readBefore == probe.initial;
    }
    else
    {
        probe.contentsMatched = probe.contentsMatched && readBefore == probe.writeTwo;
    }
    if (!testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, true, probe.address, probe.writeOne.data(), static_cast<UINT32>(probe.writeOne.size())) ||
        !testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, true, probe.address, probe.writeTwo.data(), static_cast<UINT32>(probe.writeTwo.size())))
    {
        probe.callbacksSucceeded = false;
    }
    if (!testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, false, probe.address, probe.finalRead.data(), static_cast<UINT32>(probe.finalRead.size())))
    {
        probe.callbacksSucceeded = false;
    }
    probe.contentsMatched = probe.contentsMatched && probe.finalRead == probe.writeTwo;
    ++probe.pumpCalls;
}

struct SingleCallbackProbe final
{
    void* callbackContext{};
    UINT64 address{};
    std::vector<BYTE> bytes;
    bool write{};
    bool result{};
};

void ExerciseSingleGuestMemoryCallback(void* backendContext, void* hookContext) noexcept
{
    auto& probe = *static_cast<SingleCallbackProbe*>(hookContext);
    probe.callbackContext = backendContext;
    probe.result = testing::InvokeGuestMemoryCallbackForTesting(
        backendContext, probe.write, probe.address, probe.bytes.data(), static_cast<UINT32>(probe.bytes.size()));
}

struct ApertureConcurrencyProbe final
{
    std::mutex mutex;
    std::condition_variable changed;
    void* callbackContext{};
    UINT64 address{};
    std::array<BYTE, 16> bytes{};
    bool pumpBatchReady{};
    bool batchEnding{};
    bool releasePump{};
    bool concurrentCopyEntered{};
    bool releaseConcurrentCopy{};
    std::atomic<bool> blockNextCopy{};
    bool pumpCallbacksSucceeded{true};
};

void SignalApertureBatchEnding(void* hookContext) noexcept
{
    auto& probe = *static_cast<ApertureConcurrencyProbe*>(hookContext);
    try
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        probe.batchEnding = true;
        probe.changed.notify_all();
    }
    catch (...)
    {
    }
}

void HoldPumpBatchOpen(void* backendContext, void* hookContext) noexcept
{
    auto& probe = *static_cast<ApertureConcurrencyProbe*>(hookContext);
    std::array<BYTE, 16> readBuffer{};
    if (!testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, false, probe.address, readBuffer.data(), static_cast<UINT32>(readBuffer.size())) ||
        !testing::InvokeGuestMemoryCallbackForTesting(
            backendContext, true, probe.address, probe.bytes.data(), static_cast<UINT32>(probe.bytes.size())))
    {
        probe.pumpCallbacksSucceeded = false;
    }
    try
    {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.callbackContext = backendContext;
        probe.pumpBatchReady = true;
        probe.changed.notify_all();
        probe.changed.wait(lock, [&probe] { return probe.releasePump; });
    }
    catch (...)
    {
        probe.pumpCallbacksSucceeded = false;
    }
}

void BlockOneMappedCopy(void* hookContext, bool) noexcept
{
    auto& probe = *static_cast<ApertureConcurrencyProbe*>(hookContext);
    if (!probe.blockNextCopy.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    try
    {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.concurrentCopyEntered = true;
        probe.changed.notify_all();
        probe.changed.wait(lock, [&probe] { return probe.releaseConcurrentCopy; });
    }
    catch (...)
    {
    }
}

void SetApertureTestApis(bool enabled)
{
    testing::InstallGuestMemoryApertureApiForTesting(
        enabled ? &FakeCreateGuestMemoryAperture : nullptr, enabled ? &FakeDestroyGuestMemoryAperture : nullptr);
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    testing::SetApertureCopyTestHook(nullptr, nullptr);
    testing::SetApertureBatchEndTestHook(nullptr, nullptr);
    testing::ResetGuestMemoryApertureBatchStatsForTesting();
}

std::unique_ptr<HdvPciDeviceHost> CreateStartedHost()
{
    auto host = HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId, {}, HostOwnership::VerifiedExclusive);
    const auto callbacks = GetCallbackSnapshot();
    Require(callbacks.deviceInterface.Start(callbacks.context) == S_OK, "fake HDV Start callback failed");
    return host;
}

struct ApertureRangeProbe final
{
    UINT64 largeAddress{};
    UINT64 overflowAddress{};
    std::vector<BYTE> largeBytes;
    std::array<BYTE, 16> overflowBytes{};
    bool largeResult{};
    bool overflowResult{};
};

void ExerciseRangeCallbacks(void* backendContext, void* hookContext) noexcept
{
    auto& probe = *static_cast<ApertureRangeProbe*>(hookContext);
    probe.largeResult = testing::InvokeGuestMemoryCallbackForTesting(
        backendContext, false, probe.largeAddress, probe.largeBytes.data(), static_cast<UINT32>(probe.largeBytes.size()));
    probe.overflowResult = testing::InvokeGuestMemoryCallbackForTesting(
        backendContext, false, probe.overflowAddress, probe.overflowBytes.data(), static_cast<UINT32>(probe.overflowBytes.size()));
}

void TestPumpBatchReusesAndCleansApertures()
{
    SetApertureTestApis(true);
    ResetFake();
    PumpApertureProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 4090;
    for (std::size_t index = 0; index < probe.initial.size(); ++index)
    {
        probe.initial[index] = static_cast<BYTE>(index + 1);
        probe.writeOne[index] = static_cast<BYTE>(0x20 + index);
        probe.writeTwo[index] = static_cast<BYTE>(0x60 + index);
    }
    SetFakeGuestMemory(probe.address, probe.initial.data(), static_cast<UINT32>(probe.initial.size()));
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseGuestMemoryDuringPump, &probe);

    (void)host->PumpCapture(1);
    (void)host->PumpCaptureElapsed(10'000'000);
    testing::SetAperturePumpTestHook(nullptr, nullptr);

    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(
        probe.pumpCalls == 2 && probe.callbacksSucceeded && probe.contentsMatched,
        "guest-memory copy data did not survive two pump batches");
    Require(
        stats.createCalls == 4 && stats.destroyCalls == 4,
        "pump batches did not create and destroy their apertures independently");
    Require(
        stats.mappedReadCopies == 4 && stats.mappedWriteCopies == 4,
        "same-range callbacks did not reuse read/write apertures within each batch");
    Require(
        stats.fallbackReadCalls == 0 && stats.fallbackWriteCalls == 0,
        "supported in-batch aperture copies fell back to HDV read/write calls");
    Require(stats.peakEntries == 2, "read and write permissions were not held in distinct bounded apertures");
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        Require(g_fake.activeApertures.empty(), "pump returned while an aperture remained active");
        Require(g_fake.apertureCreateLog.size() == 4, "unexpected aperture creation log size");
        for (std::size_t index = 0; index < g_fake.apertureCreateLog.size(); index += 2)
        {
            Require(g_fake.apertureCreateLog[index].writeProtected == TRUE, "guest reads did not request a read-only aperture");
            Require(
                g_fake.apertureCreateLog[index + 1].writeProtected == FALSE, "guest writes did not request a writable aperture");
        }
    }

    SingleCallbackProbe outsidePump{};
    outsidePump.address = probe.address;
    outsidePump.bytes.resize(16);
    outsidePump.write = false;
    Require(
        testing::InvokeGuestMemoryCallbackForTesting(probe.callbackContext, false, outsidePump.address, outsidePump.bytes.data(), 16),
        "guest-memory callback outside a pump failed");
    const auto afterOutsidePump = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(
        afterOutsidePump.createCalls == 4 && afterOutsidePump.fallbackReadCalls == 1,
        "apertures persisted beyond their pump batch");
    Require(host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded, "successful aperture host did not retire cleanly");
    SetApertureTestApis(false);
}

void TestOptionalApertureApiFallsBack()
{
    SetApertureTestApis(false);
    ResetFake();
    SingleCallbackProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 0x2000;
    probe.bytes.resize(16);
    SetFakeGuestMemory(probe.address, probe.bytes.data(), static_cast<UINT32>(probe.bytes.size()));
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseSingleGuestMemoryCallback, &probe);
    (void)host->PumpCapture(1);
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(probe.result, "legacy guest-memory fallback failed when aperture exports were absent");
    Require(
        stats.createCalls == 0 && stats.mappedReadCopies == 0 && stats.fallbackReadCalls == 1,
        "missing optional exports did not use the direct HDV fallback");
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded,
        "host with absent optional exports did not retire cleanly");
    SetApertureTestApis(false);
}

void TestFailedCreateAccountsForReturnedPointerAndFallsBack()
{
    SetApertureTestApis(true);
    ResetFake();
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        g_fake.createApertureResult = E_FAIL;
        g_fake.returnApertureOnCreateFailure = true;
    }
    PumpApertureProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 0x3000;
    for (std::size_t index = 0; index < probe.initial.size(); ++index)
    {
        probe.initial[index] = static_cast<BYTE>(0x10 + index);
        probe.writeOne[index] = static_cast<BYTE>(0x30 + index);
        probe.writeTwo[index] = static_cast<BYTE>(0x70 + index);
    }
    SetFakeGuestMemory(probe.address, probe.initial.data(), static_cast<UINT32>(probe.initial.size()));
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseGuestMemoryDuringPump, &probe);
    (void)host->PumpCapture(1);
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(
        probe.callbacksSucceeded && probe.contentsMatched, "direct fallback after failed aperture creation corrupted guest data");
    Require(
        stats.createCalls == 1 && stats.createFailures == 1 && stats.destroyCalls == 1,
        "non-null pointer returned with failed Create was not accounted and destroyed");
    Require(
        stats.mappedReadCopies == 0 && stats.mappedWriteCopies == 0 && stats.fallbackReadCalls == 2 && stats.fallbackWriteCalls == 2,
        "Create failure did not fall back through the direct HDV guest-memory APIs");
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        Require(g_fake.activeApertures.empty(), "failed Create left a returned aperture pointer active");
    }
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded,
        "host after aperture creation failure did not retire cleanly");
    SetApertureTestApis(false);
}

void TestMappedCopyFaultDoesNotFallback()
{
    SetApertureTestApis(true);
    ResetFake();
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        g_fake.returnInvalidApertureAddress = true;
    }
    SingleCallbackProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 0x4000;
    probe.bytes.resize(16);
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseSingleGuestMemoryCallback, &probe);
    (void)host->PumpCapture(1);
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(!probe.result, "invalid mapped pointer copy unexpectedly succeeded");
    Require(stats.createCalls == 1 && stats.destroyCalls == 1, "faulting aperture was not cleaned up at batch end");
    Require(
        stats.fallbackReadCalls == 0 && stats.mappedReadCopies == 0,
        "mapped access fault incorrectly retried through direct guest-memory API");
    Require(host->LastHostError() == E_FAIL, "mapped copy fault was not recorded on the host");
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded, "host after mapped copy fault did not retire cleanly");
    SetApertureTestApis(false);
}

void TestApertureRangeBoundsFallBack()
{
    SetApertureTestApis(true);
    ResetFake();
    ApertureRangeProbe probe{};
    probe.largeAddress = c_fakeGuestMemoryBase + 0x5000;
    probe.overflowAddress = std::numeric_limits<UINT64>::max() - 7;
    probe.largeBytes.resize(64 * 1024 + 1);
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseRangeCallbacks, &probe);
    (void)host->PumpCapture(1);
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(probe.largeResult, "oversized guest-memory range did not retain direct fallback behavior");
    Require(!probe.overflowResult, "overflowing guest-memory range unexpectedly passed the direct API");
    Require(
        stats.createCalls == 0 && stats.fallbackReadCalls == 2, "oversized or overflowing range attempted an aperture mapping");
    Require(host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded, "host after range fallback did not retire cleanly");
    SetApertureTestApis(false);
}

struct ApertureAggregateByteLimitProbe final
{
    static constexpr std::size_t c_mappingBytes = 64 * 1024;
    static constexpr std::size_t c_mappingCount = 8;
    static constexpr std::size_t c_batchByteLimit = 512 * 1024;
    static_assert(c_mappingBytes * c_mappingCount == c_batchByteLimit);
    std::array<BYTE, c_mappingBytes> mappedBuffer{};
    std::array<BYTE, c_mappingCount * 2 + 1> expectedEdges{};
    BYTE fallbackByte{};
    bool callbacksSucceeded{true};
    bool contentsMatched{true};
};

void ExerciseAggregateApertureByteLimit(void* backendContext, void* hookContext) noexcept
{
    auto& probe = *static_cast<ApertureAggregateByteLimitProbe*>(hookContext);
    for (std::size_t index = 0; index < ApertureAggregateByteLimitProbe::c_mappingCount; ++index)
    {
        const UINT64 address = c_fakeGuestMemoryBase + index * ApertureAggregateByteLimitProbe::c_mappingBytes;
        if (!testing::InvokeGuestMemoryCallbackForTesting(
                backendContext, false, address, probe.mappedBuffer.data(), static_cast<UINT32>(probe.mappedBuffer.size())))
        {
            probe.callbacksSucceeded = false;
        }
        probe.contentsMatched = probe.contentsMatched && probe.mappedBuffer.front() == probe.expectedEdges[index * 2] &&
                                probe.mappedBuffer.back() == probe.expectedEdges[index * 2 + 1];
    }

    const UINT64 uncachedAddress =
        c_fakeGuestMemoryBase + ApertureAggregateByteLimitProbe::c_mappingCount * ApertureAggregateByteLimitProbe::c_mappingBytes;
    if (!testing::InvokeGuestMemoryCallbackForTesting(backendContext, false, uncachedAddress, &probe.fallbackByte, 1))
    {
        probe.callbacksSucceeded = false;
    }
    probe.contentsMatched = probe.contentsMatched && probe.fallbackByte == probe.expectedEdges.back();
}

void TestApertureAggregateByteLimitFallsBackAndCleansUp()
{
    SetApertureTestApis(true);
    ResetFake();
    ApertureAggregateByteLimitProbe probe{};
    for (std::size_t index = 0; index < ApertureAggregateByteLimitProbe::c_mappingCount; ++index)
    {
        const UINT64 address = c_fakeGuestMemoryBase + index * ApertureAggregateByteLimitProbe::c_mappingBytes;
        probe.expectedEdges[index * 2] = static_cast<BYTE>(0x20 + index);
        probe.expectedEdges[index * 2 + 1] = static_cast<BYTE>(0x40 + index);
        SetFakeGuestMemory(address, &probe.expectedEdges[index * 2], 1);
        SetFakeGuestMemory(address + ApertureAggregateByteLimitProbe::c_mappingBytes - 1, &probe.expectedEdges[index * 2 + 1], 1);
    }
    const UINT64 uncachedAddress =
        c_fakeGuestMemoryBase + ApertureAggregateByteLimitProbe::c_mappingCount * ApertureAggregateByteLimitProbe::c_mappingBytes;
    probe.expectedEdges.back() = 0x7b;
    SetFakeGuestMemory(uncachedAddress, &probe.expectedEdges.back(), 1);

    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseAggregateApertureByteLimit, &probe);
    (void)host->PumpCapture(1);
    testing::SetAperturePumpTestHook(nullptr, nullptr);

    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(probe.callbacksSucceeded && probe.contentsMatched, "aggregate aperture fallback changed guest-memory contents");
    Require(
        stats.createCalls == ApertureAggregateByteLimitProbe::c_mappingCount && stats.destroyCalls == ApertureAggregateByteLimitProbe::c_mappingCount,
        "aggregate byte limit exceeded its mapping or cleanup count");
    Require(
        stats.mappedReadCopies == ApertureAggregateByteLimitProbe::c_mappingCount && stats.fallbackReadCalls == 1,
        "mapping beyond the 512 KiB batch budget did not use direct guest-memory fallback");
    Require(
        stats.peakEntries == ApertureAggregateByteLimitProbe::c_mappingCount,
        "aggregate byte-limit test did not fill the cache to 512 KiB");
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        Require(g_fake.activeApertures.empty(), "aggregate byte-limit batch returned with active mappings");
        Require(
            g_fake.apertureCreateLog.size() == ApertureAggregateByteLimitProbe::c_mappingCount,
            "aggregate byte-limit batch created an unexpected number of mappings");
        std::uint64_t mappedBytes{};
        for (const auto& aperture : g_fake.apertureCreateLog)
        {
            Require(
                aperture.byteCount == ApertureAggregateByteLimitProbe::c_mappingBytes, "aggregate mapping had an unexpected size");
            mappedBytes += aperture.byteCount;
        }
        Require(
            mappedBytes == ApertureAggregateByteLimitProbe::c_batchByteLimit,
            "aggregate mappings did not stop at the 512 KiB batch cap");
    }
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded,
        "host after aggregate aperture fallback did not retire cleanly");
    SetApertureTestApis(false);
}

void TestConcurrentCopyCompletesBeforeBatchDestroy()
{
    SetApertureTestApis(true);
    ResetFake();
    auto host = CreateStartedHost();
    ApertureConcurrencyProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 0x6000;
    probe.bytes.fill(0x5a);
    testing::SetAperturePumpTestHook(&HoldPumpBatchOpen, &probe);
    testing::SetApertureCopyTestHook(&BlockOneMappedCopy, &probe);
    testing::SetApertureBatchEndTestHook(&SignalApertureBatchEnding, &probe);

    int pumpResult = -1;
    bool concurrentCopyResult{};
    std::thread pumpThread([&] { pumpResult = host->PumpCapture(1); });
    auto waitForProbe = [&](bool ApertureConcurrencyProbe::* flag) {
        std::unique_lock<std::mutex> lock(probe.mutex);
        return probe.changed.wait_for(lock, std::chrono::seconds(5), [&] { return probe.*flag; });
    };
    const bool batchReady = waitForProbe(&ApertureConcurrencyProbe::pumpBatchReady);
    std::thread callbackThread;
    bool copyEntered{};
    if (batchReady)
    {
        probe.blockNextCopy.store(true, std::memory_order_release);
        callbackThread = std::thread([&] {
            std::array<BYTE, 16> readBuffer{};
            concurrentCopyResult = testing::InvokeGuestMemoryCallbackForTesting(
                probe.callbackContext, false, probe.address, readBuffer.data(), static_cast<UINT32>(readBuffer.size()));
        });
        copyEntered = waitForProbe(&ApertureConcurrencyProbe::concurrentCopyEntered);
    }

    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        probe.releasePump = true;
    }
    probe.changed.notify_all();
    const bool batchEnding = batchReady && waitForProbe(&ApertureConcurrencyProbe::batchEnding);

    bool destroyedBeforeCopyReleased{};
    if (copyEntered && batchEnding)
    {
        std::unique_lock<std::mutex> lock(g_fake.mutex);
        destroyedBeforeCopyReleased =
            g_fake.apertureChanged.wait_for(lock, std::chrono::milliseconds(100), [] { return g_fake.destroyAttemptObserved; });
    }
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        probe.releaseConcurrentCopy = true;
    }
    probe.changed.notify_all();
    pumpThread.join();
    if (callbackThread.joinable())
    {
        callbackThread.join();
    }
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    testing::SetApertureCopyTestHook(nullptr, nullptr);
    testing::SetApertureBatchEndTestHook(nullptr, nullptr);

    const auto stats = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(batchReady && copyEntered && batchEnding, "concurrent copy test did not reach its synchronization points");
    Require(!destroyedBeforeCopyReleased, "batch cleanup destroyed an aperture while a guest-memory copy held the cache lock");
    Require(
        concurrentCopyResult && probe.pumpCallbacksSucceeded && pumpResult == 0, "concurrent guest-memory copy or pump failed");
    Require(
        stats.createCalls == 2 && stats.destroyCalls == 2 && stats.destroyFailures == 0,
        "concurrent batch did not clean up both apertures");
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        Require(g_fake.activeApertures.empty(), "concurrent batch returned with an aperture still active");
    }
    Require(host->GuestStoppedAndRetire() == RetireOutcome::TeardownSucceeded, "concurrent aperture host did not retire cleanly");
    SetApertureTestApis(false);
}

void TestDestroyFailureRetainsMappingsAndPinsHost()
{
    SetApertureTestApis(true);
    ResetFake();
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        g_fake.destroyApertureResult = E_FAIL;
    }
    PumpApertureProbe probe{};
    probe.address = c_fakeGuestMemoryBase + 0x7000;
    auto host = CreateStartedHost();
    testing::SetAperturePumpTestHook(&ExerciseGuestMemoryDuringPump, &probe);
    Require(host->PumpCapture(1) == 0, "pump reported progress after aperture cleanup failed");
    testing::SetAperturePumpTestHook(nullptr, nullptr);
    const auto afterPump = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(
        afterPump.createCalls == 2 && afterPump.destroyCalls == 2 && afterPump.destroyFailures == 2,
        "batch cleanup failure was not recorded for both mappings");
    Require(host->LastHostError() == E_FAIL, "batch cleanup failure did not preserve its HRESULT");
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownFailedAndPinned,
        "host with undestroyed apertures did not remain pinned");
    const auto afterRetire = testing::GetGuestMemoryApertureBatchStatsForTesting();
    Require(afterRetire.destroyCalls == 4 && afterRetire.destroyFailures == 4, "retirement did not retry retained apertures once");
    {
        std::lock_guard<std::mutex> lock(g_fake.mutex);
        Require(g_fake.activeApertures.size() == 2, "failed aperture destruction forgot a still-active mapping");
        Require(g_fake.teardownCalls == 0, "host teardown ran while failed aperture mappings remained");
    }
    SetApertureTestApis(false);
}

void TestSuccessfulSharedHostRetiresPinned()
{
    ResetFake();
    auto host = HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId, {}, HostOwnership::UnknownOrShared);
    Require(host->Ownership() == HostOwnership::UnknownOrShared, "shared host ownership was not preserved");
    const auto callbacks = GetCallbackSnapshot();
    Require(callbacks.context != nullptr, "fake HDV API did not receive callback context");
    Require(callbacks.deviceInterface.Version == HdvPciDeviceInterfaceVersion1, "unexpected PCI callback interface version");

    HDV_PCI_PNP_ID pnpId{};
    UINT32 probedBars[HDV_PCI_BAR_COUNT]{};
    Require(callbacks.deviceInterface.Initialize(callbacks.context) == S_OK, "Initialize callback failed");
    Require(
        callbacks.deviceInterface.GetDetails(callbacks.context, &pnpId, HDV_PCI_BAR_COUNT, probedBars) == S_OK,
        "GetDetails callback failed");
    Require(
        pnpId.VendorID == wsl::virtio_sound::native::VirtioSoundDevice::c_vendorId &&
            pnpId.DeviceID == wsl::virtio_sound::native::VirtioSoundDevice::c_deviceId,
        "GetDetails returned the wrong PCI identity");
    Require(probedBars[0] == 0xffffe000U, "GetDetails returned the wrong BAR size");
    Require(callbacks.deviceInterface.Start(callbacks.context) == S_OK, "Start callback failed");

    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::OwnershipUnprovenAndPinned,
        "unknown/shared retirement did not retain the callback root");
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::OwnershipUnprovenAndPinned,
        "repeated shared retirement changed its outcome");
    Require(
        callbacks.deviceInterface.Initialize(callbacks.context) == E_ABORT, "callback admission remained open after retirement");
    host.reset();

    std::lock_guard<std::mutex> lock(g_fake.mutex);
    Require(g_fake.initializeCalls == 1 && g_fake.createCalls == 1, "unexpected fake HDV create call count");
    Require(g_fake.initializeSystemValid, "borrowed HCS_SYSTEM identity changed before HDV initialization");
    Require(g_fake.createArgumentsValid, "HDV create received the wrong PCI class/instance/interface");
    Require(g_fake.teardownCalls == 0, "unknown/shared host called HdvTeardownDeviceHost");
}

void TestFailedCreateClosesAdmissionForLateCallbacks()
{
    ResetFake(E_FAIL);
    HRESULT caughtResult = S_OK;
    try
    {
        (void)HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId);
    }
    catch (const wil::ResultException& error)
    {
        caughtResult = error.GetErrorCode();
    }
    Require(caughtResult == E_FAIL, "failed HdvCreateDeviceInstance did not preserve its HRESULT");
    const auto callbacks = GetCallbackSnapshot();
    Require(callbacks.context != nullptr, "failed create did not expose a captured callback context to the fake API");
    Require(
        callbacks.deviceInterface.Initialize(callbacks.context) == E_ABORT,
        "late callback after failed HdvCreateDeviceInstance was not rejected");
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    Require(g_fake.teardownCalls == 0, "failed create attempted an unproven HDV teardown");
}

void TestNullCreateClosesAdmissionForLateCallbacks()
{
    ResetFake(S_OK, true);
    HRESULT caughtResult = S_OK;
    try
    {
        (void)HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId);
    }
    catch (const wil::ResultException& error)
    {
        caughtResult = error.GetErrorCode();
    }
    Require(caughtResult == E_UNEXPECTED, "null HdvCreateDeviceInstance handle did not preserve E_UNEXPECTED");
    const auto callbacks = GetCallbackSnapshot();
    Require(callbacks.context != nullptr, "null create did not expose a captured callback context to the fake API");
    Require(
        callbacks.deviceInterface.Initialize(callbacks.context) == E_ABORT,
        "late callback after null HdvCreateDeviceInstance was not rejected");
    std::lock_guard<std::mutex> lock(g_fake.mutex);
    Require(g_fake.teardownCalls == 0, "null create attempted an unproven HDV teardown");
}

struct DrainProbe final
{
    std::mutex mutex;
    std::condition_variable changed;
    bool callbackEntered{};
    bool retireDisarmed{};
    bool releaseCallback{};
};

void DrainLifecycleHook(void* context, unsigned int point) noexcept
{
    auto& probe = *static_cast<DrainProbe*>(context);
    try
    {
        std::unique_lock<std::mutex> lock(probe.mutex);
        if (point == c_testHookCallbackAdmitted)
        {
            probe.callbackEntered = true;
            probe.changed.notify_all();
            probe.changed.wait(lock, [&probe] { return probe.releaseCallback; });
        }
        else if (point == c_testHookRetireDisarmed)
        {
            probe.retireDisarmed = true;
            probe.changed.notify_all();
        }
    }
    catch (...)
    {
    }
}

void TestRetirementDrainsActiveCallback()
{
    ResetFake();
    auto host = HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId);
    const auto callbacks = GetCallbackSnapshot();
    DrainProbe probe;
    testing::SetLifecycleTestHook(&DrainLifecycleHook, &probe);
    HRESULT callbackResult{E_FAIL};
    RetireOutcome retireResult{RetireOutcome::AlreadyRetired};
    std::atomic<bool> retireReturned{};
    std::thread callbackThread([&] { callbackResult = callbacks.deviceInterface.Initialize(callbacks.context); });

    auto releaseCallback = [&] {
        {
            std::lock_guard<std::mutex> lock(probe.mutex);
            probe.releaseCallback = true;
        }
        probe.changed.notify_all();
    };
    auto waitFor = [&](bool DrainProbe::* flag) {
        std::unique_lock<std::mutex> lock(probe.mutex);
        return probe.changed.wait_for(lock, std::chrono::seconds(5), [&] { return probe.*flag; });
    };

    if (!waitFor(&DrainProbe::callbackEntered))
    {
        releaseCallback();
        callbackThread.join();
        testing::SetLifecycleTestHook(nullptr, nullptr);
        throw std::runtime_error("callback did not enter the adapter guard");
    }

    std::thread retireThread([&] {
        retireResult = host->GuestStoppedAndRetire();
        retireReturned.store(true, std::memory_order_release);
    });
    if (!waitFor(&DrainProbe::retireDisarmed))
    {
        releaseCallback();
        callbackThread.join();
        retireThread.join();
        testing::SetLifecycleTestHook(nullptr, nullptr);
        throw std::runtime_error("retirement did not close callback admission");
    }

    const HRESULT lateCallbackResult = callbacks.deviceInterface.Initialize(callbacks.context);
    const bool retiredBeforeDrain = retireReturned.load(std::memory_order_acquire);
    releaseCallback();
    callbackThread.join();
    retireThread.join();
    testing::SetLifecycleTestHook(nullptr, nullptr);

    Require(lateCallbackResult == E_ABORT, "callback admission remained open while retirement drained an active callback");
    Require(!retiredBeforeDrain, "retirement returned before the active callback drained");
    Require(callbackResult == S_OK, "already-admitted callback did not finish successfully");
    Require(retireResult == RetireOutcome::OwnershipUnprovenAndPinned, "shared retirement returned the wrong outcome");
}

void TestExclusiveTeardownFailureIsNotRetried()
{
    ResetFake(S_OK, false, E_FAIL);
    auto host = HdvPciDeviceHost::CreateBorrowed(FakeSystem(), c_deviceClassId, c_deviceInstanceId, {}, HostOwnership::VerifiedExclusive);
    const auto callbacks = GetCallbackSnapshot();

    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownFailedAndPinned,
        "failed exclusive teardown returned the wrong outcome");
    Require(
        host->GuestStoppedAndRetire() == RetireOutcome::TeardownFailedAndPinned,
        "repeated failed teardown returned a different outcome");
    Require(
        callbacks.deviceInterface.Initialize(callbacks.context) == E_ABORT,
        "callback admission remained open after failed teardown");

    std::lock_guard<std::mutex> lock(g_fake.mutex);
    Require(g_fake.teardownCalls == 1, "failed HdvTeardownDeviceHost was retried");
}

} // namespace

int main()
{
    testing::InstallHdvApiForTesting(
        &FakeInitialize, &FakeTeardown, &FakeCreateDevice, &FakeReadGuestMemory, &FakeWriteGuestMemory, &FakeDeliverGuestInterrupt);
    try
    {
        TestPumpBatchReusesAndCleansApertures();
        std::cout << "PASS: per-pump aperture reuse, permissions, fallback boundary, and cleanup\n";
        TestOptionalApertureApiFallsBack();
        std::cout << "PASS: optional aperture exports preserve direct guest-memory fallback\n";
        TestFailedCreateAccountsForReturnedPointerAndFallsBack();
        std::cout << "PASS: failed aperture creation accounts for a returned pointer and falls back\n";
        TestMappedCopyFaultDoesNotFallback();
        std::cout << "PASS: mapped copy faults fail closed without direct fallback\n";
        TestApertureRangeBoundsFallBack();
        std::cout << "PASS: oversized and overflowing ranges skip aperture creation\n";
        TestApertureAggregateByteLimitFallsBackAndCleansUp();
        std::cout << "PASS: aggregate aperture byte limit falls back and cleans up\n";
        TestConcurrentCopyCompletesBeforeBatchDestroy();
        std::cout << "PASS: batch cleanup waits for concurrent mapped copies\n";
        TestSuccessfulSharedHostRetiresPinned();
        std::cout << "PASS: shared-host successful create, callback table, pinning, and repeated retirement\n";
        TestFailedCreateClosesAdmissionForLateCallbacks();
        std::cout << "PASS: failed device creation rejects late callbacks\n";
        TestNullCreateClosesAdmissionForLateCallbacks();
        std::cout << "PASS: null device creation rejects late callbacks\n";
        TestRetirementDrainsActiveCallback();
        std::cout << "PASS: retirement drains an admitted callback and rejects a later callback\n";
        TestExclusiveTeardownFailureIsNotRetried();
        std::cout << "PASS: failed exclusive teardown remains pinned and is not retried\n";
        TestDestroyFailureRetainsMappingsAndPinsHost();
        std::cout << "PASS: failed aperture destroy retains mappings and blocks host teardown\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
