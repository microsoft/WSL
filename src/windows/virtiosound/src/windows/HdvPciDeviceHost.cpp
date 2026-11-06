// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "AudioSourceTrace.hpp"
#include "HdvPciDeviceHost.hpp"

namespace wsl::virtio_sound::native::windows {
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
namespace testing {
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
} // namespace testing
#endif
namespace {
    struct HdvApi
    {
        decltype(&::HdvInitializeDeviceHost) m_initialize{};
        decltype(&::HdvTeardownDeviceHost) m_teardown{};
        decltype(&::HdvCreateDeviceInstance) m_createDevice{};
        decltype(&::HdvReadGuestMemory) m_readGuestMemory{};
        decltype(&::HdvWriteGuestMemory) m_writeGuestMemory{};
        decltype(&::HdvDeliverGuestInterrupt) m_deliverGuestInterrupt{};
        decltype(&::HdvCreateGuestMemoryAperture) m_createGuestMemoryAperture{};
        decltype(&::HdvDestroyGuestMemoryAperture) m_destroyGuestMemoryAperture{};
    };

#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
    using AperturePumpTestHook = void (*)(void* backendContext, void* hookContext) noexcept;
    using ApertureCopyTestHook = void (*)(void* hookContext, bool write) noexcept;
    using ApertureBatchEndTestHook = void (*)(void* hookContext) noexcept;
    using GuestMemoryCallbackTestInvoker = bool (*)(void* backendContext, bool write, UINT64 address, BYTE* buffer, UINT32 byteCount) noexcept;

    struct HdvApiTestState
    {
        HdvApi m_api{};
        bool m_installed{};
        wil::srwlock m_hookLock;
        _Guarded_by_(m_hookLock) void (*m_lifecycleHook)(void*, unsigned int) noexcept {};
        _Guarded_by_(m_hookLock) void* m_lifecycleHookContext {};
        _Guarded_by_(m_hookLock) AperturePumpTestHook m_aperturePumpHook {};
        _Guarded_by_(m_hookLock) void* m_aperturePumpHookContext {};
        _Guarded_by_(m_hookLock) ApertureCopyTestHook m_apertureCopyHook {};
        _Guarded_by_(m_hookLock) void* m_apertureCopyHookContext {};
        _Guarded_by_(m_hookLock) ApertureBatchEndTestHook m_apertureBatchEndHook {};
        _Guarded_by_(m_hookLock) void* m_apertureBatchEndHookContext {};
        _Guarded_by_(m_hookLock) GuestMemoryCallbackTestInvoker m_guestMemoryCallbackInvoker {};
    };

    HdvApiTestState& ApiTestState()
    {
        static auto* state = new HdvApiTestState();
        return *state;
    }

    constexpr unsigned int c_testHookCallbackAdmitted = 1;
    constexpr unsigned int c_testHookRetireDisarmed = 2;

    void NotifyLifecycleTestHook(unsigned int point) noexcept
    {
        auto& state = ApiTestState();
        void (*hook)(void*, unsigned int) noexcept {};
        void* context{};
        {
            auto lock = state.m_hookLock.lock_shared();
            hook = state.m_lifecycleHook;
            context = state.m_lifecycleHookContext;
        }
        if (hook != nullptr)
        {
            hook(context, point);
        }
    }

    void NotifyAperturePumpTestHook(void* backendContext) noexcept
    {
        auto& state = ApiTestState();
        AperturePumpTestHook hook{};
        void* context{};
        {
            auto lock = state.m_hookLock.lock_shared();
            hook = state.m_aperturePumpHook;
            context = state.m_aperturePumpHookContext;
        }
        if (hook != nullptr)
        {
            hook(backendContext, context);
        }
    }

    void NotifyApertureCopyTestHook(bool write) noexcept
    {
        auto& state = ApiTestState();
        ApertureCopyTestHook hook{};
        void* context{};
        {
            auto lock = state.m_hookLock.lock_shared();
            hook = state.m_apertureCopyHook;
            context = state.m_apertureCopyHookContext;
        }
        if (hook != nullptr)
        {
            hook(context, write);
        }
    }

    void NotifyApertureBatchEndTestHook() noexcept
    {
        auto& state = ApiTestState();
        ApertureBatchEndTestHook hook{};
        void* context{};
        {
            auto lock = state.m_hookLock.lock_shared();
            hook = state.m_apertureBatchEndHook;
            context = state.m_apertureBatchEndHookContext;
        }
        if (hook != nullptr)
        {
            hook(context);
        }
    }

#endif

    struct ApertureCountersData final
    {
        std::atomic<std::uint64_t> m_mappedReadCopies{};
        std::atomic<std::uint64_t> m_mappedWriteCopies{};
        std::atomic<std::uint64_t> m_fallbackReadCalls{};
        std::atomic<std::uint64_t> m_fallbackWriteCalls{};
        std::atomic<std::uint64_t> m_createCalls{};
        std::atomic<std::uint64_t> m_createFailures{};
        std::atomic<std::uint64_t> m_destroyCalls{};
        std::atomic<std::uint64_t> m_destroyFailures{};
        std::atomic<std::uint64_t> m_peakEntries{};
        std::atomic<std::uint64_t> m_createElapsedNanoseconds{};
        std::atomic<std::uint64_t> m_destroyElapsedNanoseconds{};
    };

    ApertureCountersData& ApertureCounters()
    {
        static ApertureCountersData counters;
        return counters;
    }

    void UpdatePeakApertureEntries(std::uint64_t entries) noexcept
    {
        auto& peak = ApertureCounters().m_peakEntries;
        std::uint64_t current = peak.load(std::memory_order_relaxed);
        while (current < entries && !peak.compare_exchange_weak(current, entries, std::memory_order_relaxed))
        {
        }
    }

    void TraceApertureSummary() noexcept
    {
        const auto& counters = ApertureCounters();
        const std::uint64_t mapped = counters.m_mappedReadCopies.load(std::memory_order_relaxed) +
                                     counters.m_mappedWriteCopies.load(std::memory_order_relaxed);
        const std::uint64_t fallback = counters.m_fallbackReadCalls.load(std::memory_order_relaxed) +
                                       counters.m_fallbackWriteCalls.load(std::memory_order_relaxed);
        detail::TraceAudioSource(L"HdvApertureCopies", mapped, fallback);
        detail::TraceAudioSource(
            L"HdvApertureMappings", counters.m_createCalls.load(std::memory_order_relaxed), counters.m_destroyCalls.load(std::memory_order_relaxed));
        detail::TraceAudioSource(
            L"HdvApertureFailures", counters.m_createFailures.load(std::memory_order_relaxed), counters.m_destroyFailures.load(std::memory_order_relaxed));
        detail::TraceAudioSource(
            L"HdvApertureTimeUs",
            counters.m_createElapsedNanoseconds.load(std::memory_order_relaxed) / 1000,
            counters.m_destroyElapsedNanoseconds.load(std::memory_order_relaxed) / 1000);
        detail::TraceAudioSource(L"HdvAperturePeakEntries", counters.m_peakEntries.load(std::memory_order_relaxed));
    }

    bool IsEmptyGuid(const GUID& value) noexcept
    {
        if (value.Data1 != 0 || value.Data2 != 0 || value.Data3 != 0)
        {
            return false;
        }
        return std::all_of(std::begin(value.Data4), std::end(value.Data4), [](BYTE byte) { return byte == 0; });
    }

    template <typename Function>
    Function Resolve(HMODULE module, const char* name)
    {
        FARPROC procedure = GetProcAddress(module, name);
        if (procedure == nullptr)
        {
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), procedure == nullptr, "vmdevicehost.dll is missing %hs", name);
        }
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress is the documented dynamic ABI lookup.
        Function result = reinterpret_cast<Function>(procedure);
#pragma warning(pop)
        return result;
    }

    template <typename Function>
    Function ResolveOptional(HMODULE module, const char* name) noexcept
    {
        FARPROC procedure = GetProcAddress(module, name);
        if (procedure == nullptr)
        {
            return nullptr;
        }
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress is the documented dynamic ABI lookup.
        Function result = reinterpret_cast<Function>(procedure);
#pragma warning(pop)
        return result;
    }

#if defined(_MSC_VER)
    int FilterMappedGuestMemoryException(unsigned int exceptionCode) noexcept
    {
        return exceptionCode == EXCEPTION_ACCESS_VIOLATION || exceptionCode == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                                       : EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    bool CopyMappedGuestMemory(void* destination, const void* source, std::size_t byteCount) noexcept
    {
#if defined(_MSC_VER)
        __try
        {
            std::memcpy(destination, source, byteCount);
            return true;
        }
        __except (FilterMappedGuestMemoryException(GetExceptionCode()))
        {
            return false;
        }
#else
        std::memcpy(destination, source, byteCount);
        return true;
#endif
    }

    const HdvApi& LoadApi()
    {
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
        auto& testState = ApiTestState();
        if (!testState.m_installed)
        {
            throw std::logic_error("The HDV adapter test API was not installed");
        }
        return testState.m_api;
#else
        static const HdvApi api = [] {
            wil::unique_hmodule module{LoadLibraryExW(L"vmdevicehost.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
            THROW_LAST_ERROR_IF(!module);
            // Keep the System32 module loaded for the process lifetime. HDV
            // callback function pointers may remain active with a pinned host.
            HdvApi result{
                Resolve<decltype(&::HdvInitializeDeviceHost)>(module.get(), "HdvInitializeDeviceHost"),
                Resolve<decltype(&::HdvTeardownDeviceHost)>(module.get(), "HdvTeardownDeviceHost"),
                Resolve<decltype(&::HdvCreateDeviceInstance)>(module.get(), "HdvCreateDeviceInstance"),
                Resolve<decltype(&::HdvReadGuestMemory)>(module.get(), "HdvReadGuestMemory"),
                Resolve<decltype(&::HdvWriteGuestMemory)>(module.get(), "HdvWriteGuestMemory"),
                Resolve<decltype(&::HdvDeliverGuestInterrupt)>(module.get(), "HdvDeliverGuestInterrupt"),
                ResolveOptional<decltype(&::HdvCreateGuestMemoryAperture)>(module.get(), "HdvCreateGuestMemoryAperture"),
                ResolveOptional<decltype(&::HdvDestroyGuestMemoryAperture)>(module.get(), "HdvDestroyGuestMemoryAperture")};
            (void)module.release();
            return result;
        }();
        return api;
#endif
    }

} // namespace

#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
namespace testing {
    void InstallHdvApiForTesting(
        decltype(&::HdvInitializeDeviceHost) initialize,
        decltype(&::HdvTeardownDeviceHost) teardown,
        decltype(&::HdvCreateDeviceInstance) createDevice,
        decltype(&::HdvReadGuestMemory) readGuestMemory,
        decltype(&::HdvWriteGuestMemory) writeGuestMemory,
        decltype(&::HdvDeliverGuestInterrupt) deliverGuestInterrupt)
    {
        auto& state = ApiTestState();
        state.m_api = HdvApi{initialize, teardown, createDevice, readGuestMemory, writeGuestMemory, deliverGuestInterrupt};
        state.m_installed = true;
    }

    void InstallGuestMemoryApertureApiForTesting(decltype(&::HdvCreateGuestMemoryAperture) createAperture, decltype(&::HdvDestroyGuestMemoryAperture) destroyAperture)
    {
        auto& state = ApiTestState();
        auto lock = state.m_hookLock.lock_exclusive();
        state.m_api.m_createGuestMemoryAperture = createAperture;
        state.m_api.m_destroyGuestMemoryAperture = destroyAperture;
    }

    void SetAperturePumpTestHook(AperturePumpTestHook hook, void* context)
    {
        auto& state = ApiTestState();
        auto lock = state.m_hookLock.lock_exclusive();
        state.m_aperturePumpHook = hook;
        state.m_aperturePumpHookContext = context;
    }

    void SetApertureCopyTestHook(ApertureCopyTestHook hook, void* context)
    {
        auto& state = ApiTestState();
        auto lock = state.m_hookLock.lock_exclusive();
        state.m_apertureCopyHook = hook;
        state.m_apertureCopyHookContext = context;
    }

    void SetApertureBatchEndTestHook(ApertureBatchEndTestHook hook, void* context)
    {
        auto& state = ApiTestState();
        auto lock = state.m_hookLock.lock_exclusive();
        state.m_apertureBatchEndHook = hook;
        state.m_apertureBatchEndHookContext = context;
    }

    bool InvokeGuestMemoryCallbackForTesting(void* backendContext, bool write, UINT64 address, BYTE* buffer, UINT32 byteCount) noexcept
    {
        auto& state = ApiTestState();
        GuestMemoryCallbackTestInvoker invoker{};
        {
            auto lock = state.m_hookLock.lock_shared();
            invoker = state.m_guestMemoryCallbackInvoker;
        }
        return invoker != nullptr && invoker(backendContext, write, address, buffer, byteCount);
    }

    GuestMemoryApertureBatchStats GetGuestMemoryApertureBatchStatsForTesting() noexcept
    {
        const auto& counters = ApertureCounters();
        return {
            counters.m_mappedReadCopies.load(std::memory_order_relaxed),
            counters.m_mappedWriteCopies.load(std::memory_order_relaxed),
            counters.m_fallbackReadCalls.load(std::memory_order_relaxed),
            counters.m_fallbackWriteCalls.load(std::memory_order_relaxed),
            counters.m_createCalls.load(std::memory_order_relaxed),
            counters.m_createFailures.load(std::memory_order_relaxed),
            counters.m_destroyCalls.load(std::memory_order_relaxed),
            counters.m_destroyFailures.load(std::memory_order_relaxed),
            counters.m_peakEntries.load(std::memory_order_relaxed),
            counters.m_createElapsedNanoseconds.load(std::memory_order_relaxed),
            counters.m_destroyElapsedNanoseconds.load(std::memory_order_relaxed)};
    }

    void ResetGuestMemoryApertureBatchStatsForTesting() noexcept
    {
        auto& counters = ApertureCounters();
        counters.m_mappedReadCopies.store(0, std::memory_order_relaxed);
        counters.m_mappedWriteCopies.store(0, std::memory_order_relaxed);
        counters.m_fallbackReadCalls.store(0, std::memory_order_relaxed);
        counters.m_fallbackWriteCalls.store(0, std::memory_order_relaxed);
        counters.m_createCalls.store(0, std::memory_order_relaxed);
        counters.m_createFailures.store(0, std::memory_order_relaxed);
        counters.m_destroyCalls.store(0, std::memory_order_relaxed);
        counters.m_destroyFailures.store(0, std::memory_order_relaxed);
        counters.m_peakEntries.store(0, std::memory_order_relaxed);
        counters.m_createElapsedNanoseconds.store(0, std::memory_order_relaxed);
        counters.m_destroyElapsedNanoseconds.store(0, std::memory_order_relaxed);
    }

    void SetLifecycleTestHook(void (*hook)(void*, unsigned int) noexcept, void* context)
    {
        auto& state = ApiTestState();
        auto lock = state.m_hookLock.lock_exclusive();
        state.m_lifecycleHook = hook;
        state.m_lifecycleHookContext = context;
    }
} // namespace testing
#endif

class HdvPciDeviceHost::BackendState final
{
public:
    static constexpr std::uint64_t c_guestMemoryPageBytes = 4096;
    static constexpr std::uint32_t c_maxApertureEntries = 32;
    static constexpr std::uint32_t c_maxApertureBytes = 512 * 1024;
    static constexpr std::uint32_t c_maxSingleApertureBytes = 64 * 1024;

    struct GuestMemoryApertureRange final
    {
        std::uint64_t m_guestPhysicalAddress{};
        std::uint32_t m_byteCount{};
        std::uint32_t m_guestOffset{};
        bool m_writeProtected{};
    };

    struct GuestMemoryAperture final
    {
        std::uint64_t m_guestPhysicalAddress{};
        std::uint32_t m_byteCount{};
        bool m_writeProtected{};
        PVOID m_mappedAddress{};
    };

    enum class ApertureAttempt : std::uint8_t
    {
        NotUsed,
        Copied,
        CopyFault
    };

    BackendState(HCS_SYSTEM system, const GUID& classId, const GUID& instanceId, std::shared_ptr<PcmSource> source, HostOwnership hostOwnership) :
        m_borrowedSystem(system),
        m_deviceClassId(classId),
        m_deviceInstanceId(instanceId),
        m_ownership(hostOwnership),
        m_pcmSource(std::move(source))
    {
        m_guestMemoryApertures.reserve(c_maxApertureEntries);
        m_retainedGuestMemoryApertures.reserve(c_maxApertureEntries);
        m_interface.Version = HdvPciDeviceInterfaceVersion1;
        m_interface.Initialize = &OnInitialize;
        m_interface.Teardown = &OnTeardown;
        m_interface.SetConfiguration = &OnSetConfiguration;
        m_interface.GetDetails = &OnGetDetails;
        m_interface.Start = &OnStart;
        m_interface.Stop = &OnStop;
        m_interface.ReadConfigSpace = &OnReadConfig;
        m_interface.WriteConfigSpace = &OnWriteConfig;
        m_interface.ReadInterceptedMemory = &OnReadMemory;
        m_interface.WriteInterceptedMemory = &OnWriteMemory;

        GuestMemoryCallbacks memory{this, &ReadGuestMemory, &WriteGuestMemory};
        InterruptCallbacks interruptCallbacks{this, &DeliverMsi, nullptr};
        m_device = std::make_unique<VirtioSoundDevice>(memory, interruptCallbacks, m_pcmSource);
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
        auto& testState = ApiTestState();
        auto lock = testState.m_hookLock.lock_exclusive();
        testState.m_guestMemoryCallbackInvoker = &InvokeGuestMemoryCallbackForTesting;
#endif
    }

    static std::vector<std::shared_ptr<BackendState>>& PinnedStates()
    {
        // Deliberately immortal: static destructors must not release callback
        // contexts during CRT shutdown while an unproven HDV host may still
        // retain the interface pointer.
        static auto* states = new std::vector<std::shared_ptr<BackendState>>();
        return *states;
    }

    static wil::srwlock& PinMutex()
    {
        static wil::srwlock mutex;
        return mutex;
    }

    static void Pin(const std::shared_ptr<BackendState>& state)
    {
        auto lock = PinMutex().lock_exclusive();
        auto& states = PinnedStates();
        for (const auto& existing : states)
        {
            if (existing.get() == state.get())
            {
                return;
            }
        }
        states.push_back(state);
    }

    static void Unpin(BackendState* state)
    {
        auto lock = PinMutex().lock_exclusive();
        auto& states = PinnedStates();
        states.erase(
            std::remove_if(states.begin(), states.end(), [state](const auto& value) { return value.get() == state; }), states.end());
    }

    HCS_SYSTEM m_borrowedSystem{}; // borrowed only; never closed here
    GUID m_deviceClassId{};
    GUID m_deviceInstanceId{};
    HostOwnership m_ownership{HostOwnership::UnknownOrShared};
    std::shared_ptr<PcmSource> m_pcmSource;
    std::unique_ptr<VirtioSoundDevice> m_device;
    HDV_PCI_DEVICE_INTERFACE m_interface{};
    HDV_HOST m_deviceHost{};
    HDV_DEVICE m_deviceHandle{};
    std::atomic<HRESULT> m_creationResult{E_PENDING};
    std::atomic<HRESULT> m_lastHostError{S_OK};
    mutable std::mutex m_lifecycleMutex;
    std::condition_variable m_lifecycleChanged;
    _Guarded_by_(m_lifecycleMutex) std::uint32_t m_activeOperations {};
    _Guarded_by_(m_lifecycleMutex) bool m_started {};
    _Guarded_by_(m_lifecycleMutex) bool m_guestStopped {};
    _Guarded_by_(m_lifecycleMutex) bool m_callbacksDisarmed {};
    _Guarded_by_(m_lifecycleMutex) bool m_teardownAttempted {};
    _Guarded_by_(m_lifecycleMutex) bool m_teardownSucceeded {};
    _Guarded_by_(m_lifecycleMutex) RetireOutcome m_retireOutcome { RetireOutcome::OwnershipUnprovenAndPinned };
    std::mutex m_apertureBatchMutex;
    std::mutex m_guestMemoryApertureMutex;
    _Guarded_by_(m_guestMemoryApertureMutex) bool m_guestMemoryApertureBatchActive {};
    _Guarded_by_(m_guestMemoryApertureMutex) bool m_guestMemoryApertureBatchFailed {};
    _Guarded_by_(m_guestMemoryApertureMutex) bool m_guestMemoryAperturesDisabled {};
    _Guarded_by_(m_guestMemoryApertureMutex) std::uint32_t m_guestMemoryApertureBytes {};
    _Guarded_by_(m_guestMemoryApertureMutex) std::vector<GuestMemoryAperture> m_guestMemoryApertures;
    _Guarded_by_(m_guestMemoryApertureMutex) std::vector<GuestMemoryAperture> m_retainedGuestMemoryApertures;

    bool TryEnter(bool requireStarted = false) noexcept
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_callbacksDisarmed || (requireStarted && (!m_started || m_guestStopped)))
        {
            return false;
        }
        ++m_activeOperations;
        return true;
    }

    void Exit() noexcept
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_activeOperations != 0)
        {
            --m_activeOperations;
        }
        if (m_activeOperations == 0)
        {
            m_lifecycleChanged.notify_all();
        }
    }

    void FailClosedAfterRegistration() noexcept
    {
        try
        {
            std::unique_lock<std::mutex> lock(m_lifecycleMutex);
            m_guestStopped = true;
            m_started = false;
            m_callbacksDisarmed = true;
            m_lifecycleChanged.wait(lock, [this] { return m_activeOperations == 0; });
        }
        catch (...)
        {
            // Admission is closed before waiting. Keep the m_state pinned even
            // if a synchronization failure prevents confirming the drain.
            m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
        }
    }

    struct Operation final
    {
        explicit Operation(BackendState* value, bool requireStarted = false) noexcept :
            m_state(value), m_entered(m_state != nullptr && m_state->TryEnter(requireStarted))
        {
        }
        ~Operation()
        {
            if (m_entered)
            {
                m_state->Exit();
            }
        }
        BackendState* m_state{};
        bool m_entered{};
        explicit operator bool() const noexcept
        {
            return m_entered;
        }
    };

    struct ApertureBatch final
    {
        explicit ApertureBatch(BackendState& state) : m_state(state), m_batchLock(state.m_apertureBatchMutex)
        {
            std::lock_guard<std::mutex> cacheLock(m_state.m_guestMemoryApertureMutex);
            m_state.m_guestMemoryApertureBatchActive = true;
            m_state.m_guestMemoryApertureBatchFailed = false;
            m_started = true;
        }

        ~ApertureBatch()
        {
            (void)Finish();
        }

        bool Finish() noexcept
        {
            if (!m_started || m_finished)
            {
                return m_cleanupSucceeded;
            }
            m_finished = true;
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
            NotifyApertureBatchEndTestHook();
#endif
            try
            {
                const HdvApi& api = LoadApi();
                std::lock_guard<std::mutex> cacheLock(m_state.m_guestMemoryApertureMutex);
                m_state.m_guestMemoryApertureBatchActive = false;
                m_cleanupSucceeded = m_state.DestroyBatchAperturesLocked(api);
                return m_cleanupSucceeded;
            }
            catch (...)
            {
                try
                {
                    std::lock_guard<std::mutex> cacheLock(m_state.m_guestMemoryApertureMutex);
                    m_state.m_guestMemoryApertureBatchActive = false;
                    m_state.m_guestMemoryAperturesDisabled = true;
                }
                catch (...)
                {
                    // Keep the host fail-closed if the cache lock itself fails.
                }
                m_state.m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
                m_cleanupSucceeded = false;
                return false;
            }
        }

        BackendState& m_state;
        std::unique_lock<std::mutex> m_batchLock;
        bool m_started{};
        bool m_finished{};
        bool m_cleanupSucceeded{true};
    };

    static bool TryMakeGuestMemoryApertureRange(std::uint64_t address, std::size_t byteCount, bool writeProtected, GuestMemoryApertureRange& range) noexcept
    {
        constexpr std::uint64_t pageMask = c_guestMemoryPageBytes - 1;
        constexpr std::uint64_t maxAddress = std::numeric_limits<std::uint64_t>::max();
        if (byteCount == 0 || byteCount > c_maxSingleApertureBytes || byteCount > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        const std::uint64_t length = static_cast<std::uint64_t>(byteCount);
        if (address > maxAddress - length)
        {
            return false;
        }
        const std::uint64_t endAddress = address + length;
        if (endAddress > maxAddress - pageMask)
        {
            return false;
        }
        const std::uint64_t alignedBase = address & ~pageMask;
        const std::uint64_t alignedEnd = (endAddress + pageMask) & ~pageMask;
        if (alignedEnd <= alignedBase)
        {
            return false;
        }
        const std::uint64_t mappedBytes = alignedEnd - alignedBase;
        const std::uint64_t offset = address - alignedBase;
        if (mappedBytes > c_maxSingleApertureBytes || mappedBytes > std::numeric_limits<std::uint32_t>::max() ||
            offset > std::numeric_limits<std::uint32_t>::max() || offset + length > mappedBytes)
        {
            return false;
        }
        range = GuestMemoryApertureRange{alignedBase, static_cast<std::uint32_t>(mappedBytes), static_cast<std::uint32_t>(offset), writeProtected};
        return true;
    }

    HRESULT DestroyGuestMemoryAperture(const HdvApi& api, const GuestMemoryAperture& aperture) noexcept
    {
        const auto start = std::chrono::steady_clock::now();
        const HDV_DEVICE handle = GetDeviceHandle();
        const HRESULT result = api.m_destroyGuestMemoryAperture == nullptr || handle == nullptr
                                   ? E_HANDLE
                                   : api.m_destroyGuestMemoryAperture(handle, aperture.m_mappedAddress);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        ApertureCounters().m_destroyCalls.fetch_add(1, std::memory_order_relaxed);
        ApertureCounters().m_destroyElapsedNanoseconds.fetch_add(
            static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)), std::memory_order_relaxed);
        if (FAILED(result))
        {
            ApertureCounters().m_destroyFailures.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    bool DestroyBatchAperturesLocked(const HdvApi& api) noexcept
    {
        bool succeeded = true;
        for (const GuestMemoryAperture& aperture : m_guestMemoryApertures)
        {
            const HRESULT result = DestroyGuestMemoryAperture(api, aperture);
            if (FAILED(result))
            {
                m_lastHostError.store(result, std::memory_order_relaxed);
                m_guestMemoryAperturesDisabled = true;
                succeeded = false;
                m_retainedGuestMemoryApertures.push_back(aperture);
            }
        }
        m_guestMemoryApertures.clear();
        m_guestMemoryApertureBytes = 0;
        return succeeded;
    }

    bool FlushGuestMemoryAperturesForRetirement() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> cacheLock(m_guestMemoryApertureMutex);
            m_guestMemoryApertureBatchActive = false;
            const HdvApi& api = LoadApi();
            const bool currentSucceeded = DestroyBatchAperturesLocked(api);
            bool retainedSucceeded = true;
            std::size_t index{};
            while (index < m_retainedGuestMemoryApertures.size())
            {
                const HRESULT result = DestroyGuestMemoryAperture(api, m_retainedGuestMemoryApertures[index]);
                if (SUCCEEDED(result))
                {
                    m_retainedGuestMemoryApertures.erase(m_retainedGuestMemoryApertures.begin() + index);
                }
                else
                {
                    m_lastHostError.store(result, std::memory_order_relaxed);
                    retainedSucceeded = false;
                    ++index;
                }
            }
            return currentSucceeded && retainedSucceeded;
        }
        catch (...)
        {
            try
            {
                std::lock_guard<std::mutex> cacheLock(m_guestMemoryApertureMutex);
                m_guestMemoryApertureBatchActive = false;
                m_guestMemoryAperturesDisabled = true;
            }
            catch (...)
            {
                // The atomic error still records fail-closed cleanup failure.
            }
            m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
            return false;
        }
    }

    ApertureAttempt TryCopyThroughGuestMemoryAperture(const HdvApi& api, std::uint64_t address, BYTE* buffer, std::size_t byteCount, bool write) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> cacheLock(m_guestMemoryApertureMutex);
            if (!m_guestMemoryApertureBatchActive || m_guestMemoryApertureBatchFailed || m_guestMemoryAperturesDisabled ||
                api.m_createGuestMemoryAperture == nullptr || api.m_destroyGuestMemoryAperture == nullptr)
            {
                return ApertureAttempt::NotUsed;
            }

            GuestMemoryApertureRange range{};
            if (!TryMakeGuestMemoryApertureRange(address, byteCount, !write, range))
            {
                return ApertureAttempt::NotUsed;
            }

            auto aperture =
                std::find_if(m_guestMemoryApertures.begin(), m_guestMemoryApertures.end(), [&range](const GuestMemoryAperture& existing) {
                    return existing.m_guestPhysicalAddress == range.m_guestPhysicalAddress &&
                           existing.m_byteCount == range.m_byteCount && existing.m_writeProtected == range.m_writeProtected;
                });
            if (aperture == m_guestMemoryApertures.end())
            {
                if (m_guestMemoryApertures.size() >= c_maxApertureEntries || range.m_byteCount > c_maxApertureBytes - m_guestMemoryApertureBytes)
                {
                    return ApertureAttempt::NotUsed;
                }

                PVOID mappedAddress{};
                const auto createStart = std::chrono::steady_clock::now();
                ApertureCounters().m_createCalls.fetch_add(1, std::memory_order_relaxed);
                const HDV_DEVICE handle = GetDeviceHandle();
                const HRESULT createResult =
                    handle == nullptr
                        ? E_HANDLE
                        : api.m_createGuestMemoryAperture(
                              handle, range.m_guestPhysicalAddress, range.m_byteCount, range.m_writeProtected ? TRUE : FALSE, &mappedAddress);
                const auto createElapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - createStart).count();
                ApertureCounters().m_createElapsedNanoseconds.fetch_add(
                    static_cast<std::uint64_t>(std::max<std::int64_t>(createElapsed, 0)), std::memory_order_relaxed);
                if (FAILED(createResult) || mappedAddress == nullptr)
                {
                    // Although a failed API call should not normally return a
                    // usable aperture, retain any non-null pointer it produced
                    // so cleanup still accounts for every possible mapping.
                    if (mappedAddress != nullptr)
                    {
                        m_guestMemoryApertures.push_back(GuestMemoryAperture{
                            range.m_guestPhysicalAddress, range.m_byteCount, range.m_writeProtected, mappedAddress});
                        m_guestMemoryApertureBytes += range.m_byteCount;
                        UpdatePeakApertureEntries(m_guestMemoryApertures.size());
                    }
                    m_guestMemoryApertureBatchFailed = true;
                    m_lastHostError.store(FAILED(createResult) ? createResult : E_UNEXPECTED, std::memory_order_relaxed);
                    ApertureCounters().m_createFailures.fetch_add(1, std::memory_order_relaxed);
                    return ApertureAttempt::NotUsed;
                }

                m_guestMemoryApertures.push_back(
                    GuestMemoryAperture{range.m_guestPhysicalAddress, range.m_byteCount, range.m_writeProtected, mappedAddress});
                m_guestMemoryApertureBytes += range.m_byteCount;
                UpdatePeakApertureEntries(m_guestMemoryApertures.size());
                aperture = std::prev(m_guestMemoryApertures.end());
            }

            BYTE* mappedBytes = static_cast<BYTE*>(aperture->m_mappedAddress) + range.m_guestOffset;
            MemoryBarrier();
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
            NotifyApertureCopyTestHook(write);
#endif
            const bool copied = write ? CopyMappedGuestMemory(mappedBytes, buffer, byteCount)
                                      : CopyMappedGuestMemory(buffer, mappedBytes, byteCount);
            MemoryBarrier();
            if (!copied)
            {
                m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
                return ApertureAttempt::CopyFault;
            }
            (write ? ApertureCounters().m_mappedWriteCopies : ApertureCounters().m_mappedReadCopies).fetch_add(1, std::memory_order_relaxed);
            return ApertureAttempt::Copied;
        }
        catch (...)
        {
            m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
            return ApertureAttempt::CopyFault;
        }
    }

    HDV_DEVICE GetDeviceHandle() noexcept
    {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        return m_deviceHandle;
    }

    static bool ReadGuestMemory(void* context, std::uint64_t address, std::uint8_t* destination, std::size_t byteCount) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        try
        {
            if (state == nullptr || (byteCount != 0 && destination == nullptr) || byteCount > MAXDWORD)
            {
                return false;
            }
            HDV_DEVICE handle = state->GetDeviceHandle();
            const HdvApi& api = LoadApi();
            if (handle != nullptr)
            {
                const ApertureAttempt attempt = state->TryCopyThroughGuestMemoryAperture(api, address, destination, byteCount, false);
                if (attempt == ApertureAttempt::Copied)
                {
                    return true;
                }
                if (attempt == ApertureAttempt::CopyFault)
                {
                    return false;
                }
            }
            ApertureCounters().m_fallbackReadCalls.fetch_add(1, std::memory_order_relaxed);
            const HRESULT result =
                handle == nullptr ? E_HANDLE : api.m_readGuestMemory(handle, address, static_cast<UINT32>(byteCount), destination);
            if (FAILED_LOG(result))
            {
                state->m_lastHostError.store(result, std::memory_order_relaxed);
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool WriteGuestMemory(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t byteCount) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        try
        {
            if (state == nullptr || (byteCount != 0 && source == nullptr) || byteCount > MAXDWORD)
            {
                return false;
            }
            HDV_DEVICE handle = state->GetDeviceHandle();
            const HdvApi& api = LoadApi();
            if (handle != nullptr)
            {
                const ApertureAttempt attempt =
                    state->TryCopyThroughGuestMemoryAperture(api, address, const_cast<std::uint8_t*>(source), byteCount, true);
                if (attempt == ApertureAttempt::Copied)
                {
                    return true;
                }
                if (attempt == ApertureAttempt::CopyFault)
                {
                    return false;
                }
            }
            ApertureCounters().m_fallbackWriteCalls.fetch_add(1, std::memory_order_relaxed);
            const HRESULT result =
                handle == nullptr ? E_HANDLE : api.m_writeGuestMemory(handle, address, static_cast<UINT32>(byteCount), source);
            if (FAILED_LOG(result))
            {
                state->m_lastHostError.store(result, std::memory_order_relaxed);
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
    static bool InvokeGuestMemoryCallbackForTesting(void* context, bool write, UINT64 address, BYTE* buffer, UINT32 byteCount) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        Operation operation(state);
        if (!operation || (byteCount != 0 && buffer == nullptr))
        {
            return false;
        }
        return write ? WriteGuestMemory(context, address, buffer, byteCount) : ReadGuestMemory(context, address, buffer, byteCount);
    }
#endif

    static bool DeliverMsi(void* context, std::uint64_t address, std::uint32_t data) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        try
        {
            if (state == nullptr)
            {
                return false;
            }
            HDV_DEVICE handle = state->GetDeviceHandle();
            HRESULT hr = handle == nullptr ? E_HANDLE : LoadApi().m_deliverGuestInterrupt(handle, address, data);
            state->m_lastHostError.store(hr, std::memory_order_relaxed);
            return SUCCEEDED(hr);
        }
        catch (...)
        {
            return false;
        }
    }

    template <typename Callback>
    static HRESULT Guard(PVOID context, Callback&& callback) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        Operation operation(state);
        if (!operation)
        {
            return E_ABORT;
        }
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
        NotifyLifecycleTestHook(c_testHookCallbackAdmitted);
#endif
        try
        {
            HRESULT result = callback(*state);
            if (FAILED_LOG(result))
            {
                state->m_lastHostError.store(result, std::memory_order_relaxed);
            }
            return result;
        }
        catch (...)
        {
            state->m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
            return E_FAIL;
        }
    }

    static HRESULT CALLBACK OnInitialize(PVOID context) noexcept
    {
        return Guard(context, [](BackendState&) { return S_OK; });
    }

    static void CALLBACK OnTeardown(PVOID context) noexcept
    {
        LifecycleReset(context);
    }

    static HRESULT CALLBACK OnSetConfiguration(PVOID context, UINT32 count, const LPCWSTR* values) noexcept
    {
        return Guard(context, [count, values](BackendState&) {
            if (count > 64 || (count != 0 && values == nullptr))
            {
                return E_INVALIDARG;
            }
            return S_OK;
        });
    }

    static HRESULT CALLBACK OnGetDetails(PVOID context, PHDV_PCI_PNP_ID pnpId, UINT32 probedBarsCount, UINT32* probedBars) noexcept
    {
        return Guard(context, [pnpId, probedBarsCount, probedBars](BackendState& state) {
            if (pnpId == nullptr || probedBarsCount < HDV_PCI_BAR_COUNT || probedBars == nullptr)
            {
                return E_INVALIDARG;
            }
            pnpId->VendorID = VirtioSoundDevice::c_vendorId;
            pnpId->DeviceID = VirtioSoundDevice::c_deviceId;
            pnpId->RevisionID = 1;
            pnpId->ProgIf = 0;
            pnpId->SubClass = 1;
            pnpId->BaseClass = 4;
            pnpId->SubVendorID = VirtioSoundDevice::c_vendorId;
            pnpId->SubSystemID = 25;
            std::fill_n(probedBars, HDV_PCI_BAR_COUNT, 0U);
            probedBars[0] = 0xffffe000U;
            state.m_lastHostError.store(S_OK, std::memory_order_relaxed);
            return S_OK;
        });
    }

    static HRESULT CALLBACK OnStart(PVOID context) noexcept
    {
        return Guard(context, [](BackendState& state) {
            std::lock_guard<std::mutex> lock(state.m_lifecycleMutex);
            if (state.m_guestStopped)
            {
                return E_ABORT;
            }
            state.m_started = true;
            state.m_lastHostError.store(S_OK, std::memory_order_relaxed);
            return S_OK;
        });
    }

    static void CALLBACK OnStop(PVOID context) noexcept
    {
        LifecycleReset(context);
    }

    static HRESULT CALLBACK OnReadConfig(PVOID context, UINT32 offset, UINT32* value) noexcept
    {
        if (value == nullptr)
        {
            return E_POINTER;
        }
        *value = 0;
        return Guard(context, [offset, value](BackendState& state) {
            *value = state.m_device->ReadConfig(offset, 4);
            return S_OK;
        });
    }

    static HRESULT CALLBACK OnWriteConfig(PVOID context, UINT32 offset, UINT32 value) noexcept
    {
        return Guard(context, [offset, value](BackendState& state) {
            state.m_device->WriteConfig(offset, value, 4);
            return S_OK;
        });
    }

    static HRESULT CALLBACK OnReadMemory(PVOID context, HDV_PCI_BAR_SELECTOR barIndex, UINT64 offset, UINT64 length, BYTE* value) noexcept
    {
        if (barIndex != HDV_PCI_BAR0 || value == nullptr || length > 8)
        {
            return E_INVALIDARG;
        }
        return Guard(context, [offset, length, value](BackendState& state) {
            state.m_device->ReadBar(offset, value, static_cast<std::size_t>(length));
            return S_OK;
        });
    }

    static HRESULT CALLBACK OnWriteMemory(PVOID context, HDV_PCI_BAR_SELECTOR barIndex, UINT64 offset, UINT64 length, const BYTE* value) noexcept
    {
        if (barIndex != HDV_PCI_BAR0 || value == nullptr || length > 8)
        {
            return E_INVALIDARG;
        }
        return Guard(context, [offset, length, value](BackendState& state) {
            state.m_device->WriteBar(offset, value, static_cast<std::size_t>(length));
            return S_OK;
        });
    }

    static void LifecycleReset(PVOID context) noexcept
    {
        auto* state = static_cast<BackendState*>(context);
        Operation operation(state);
        if (!operation)
        {
            return;
        }
        try
        {
            state->m_device->Reset();
            std::lock_guard<std::mutex> lock(state->m_lifecycleMutex);
            state->m_started = false;
        }
        catch (...)
        {
            state->m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
        }
    }
};

HdvPciDeviceHost::HdvPciDeviceHost(std::shared_ptr<BackendState> state) noexcept : m_state(std::move(state))
{
}

HdvPciDeviceHost::~HdvPciDeviceHost() = default;

std::unique_ptr<HdvPciDeviceHost> HdvPciDeviceHost::CreateBorrowed(
    HCS_SYSTEM borrowedSystem, const GUID& deviceClassId, const GUID& deviceInstanceId, std::shared_ptr<PcmSource> source, HostOwnership ownership)
{
    if (borrowedSystem == nullptr)
    {
        throw std::invalid_argument("A borrowed HCS_SYSTEM handle is required");
    }
    if (IsEmptyGuid(deviceClassId) || IsEmptyGuid(deviceInstanceId))
    {
        throw std::invalid_argument("Device class and instance GUIDs must not be empty");
    }

    const HdvApi& api = LoadApi();
    auto state = std::make_shared<BackendState>(borrowedSystem, deviceClassId, deviceInstanceId, std::move(source), ownership);
    HRESULT hr = api.m_initialize(borrowedSystem, &state->m_deviceHost);
    state->m_creationResult.store(hr, std::memory_order_relaxed);
    if (hr < 0)
    {
        state->m_lastHostError.store(hr, std::memory_order_relaxed);
        if (state->m_deviceHost != nullptr)
        {
            BackendState::Pin(state); // anomalous handle: retain; never guess cleanup.
        }
        THROW_HR_IF_MSG(hr, hr < 0, "HdvInitializeDeviceHost");
    }
    if (state->m_deviceHost == nullptr)
    {
        state->m_lastHostError.store(E_UNEXPECTED, std::memory_order_relaxed);
        THROW_HR(E_UNEXPECTED);
    }

    // Root callback context before HDV can call the interface. It remains
    // rooted if device creation fails or ownership cannot later be proven.
    BackendState::Pin(state);
    HDV_DEVICE deviceHandle{};
    try
    {
        hr = api.m_createDevice(
            state->m_deviceHost, HdvDeviceTypePCI, &state->m_deviceClassId, &state->m_deviceInstanceId, &state->m_interface, state.get(), &deviceHandle);
    }
    catch (...)
    {
        state->FailClosedAfterRegistration();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(state->m_lifecycleMutex);
        state->m_deviceHandle = deviceHandle;
    }
    state->m_creationResult.store(hr, std::memory_order_relaxed);
    if (hr < 0)
    {
        state->m_lastHostError.store(hr, std::memory_order_relaxed);
        state->FailClosedAfterRegistration();
        THROW_HR_IF_MSG(hr, hr < 0, "HdvCreateDeviceInstance");
    }
    if (deviceHandle == nullptr)
    {
        state->m_lastHostError.store(E_UNEXPECTED, std::memory_order_relaxed);
        state->FailClosedAfterRegistration();
        THROW_HR(E_UNEXPECTED);
    }
    try
    {
        // Copy the shared owner until allocation succeeds so the failure path
        // can still fail-close the process-pinned callback context.
        return std::unique_ptr<HdvPciDeviceHost>(new HdvPciDeviceHost(state));
    }
    catch (...)
    {
        state->FailClosedAfterRegistration();
        throw;
    }
}

RetireOutcome HdvPciDeviceHost::GuestStoppedAndRetire() noexcept
{
    if (!m_state)
    {
        return RetireOutcome::AlreadyRetired;
    }
    auto& state = *m_state;
    std::unique_lock<std::mutex> lock(state.m_lifecycleMutex);
    if (state.m_guestStopped)
    {
        return state.m_teardownSucceeded ? RetireOutcome::AlreadyRetired : state.m_retireOutcome;
    }
    state.m_guestStopped = true;
    state.m_started = false;
    state.m_callbacksDisarmed = true; // close callback admission before draining active calls
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
    NotifyLifecycleTestHook(c_testHookRetireDisarmed);
#endif
    state.m_lifecycleChanged.wait(lock, [&state] { return state.m_activeOperations == 0; });
    state.m_retireOutcome = state.m_ownership == HostOwnership::VerifiedExclusive ? RetireOutcome::TeardownFailedAndPinned
                                                                                  : RetireOutcome::OwnershipUnprovenAndPinned;
    lock.unlock();
    const bool aperturesCleaned = state.FlushGuestMemoryAperturesForRetirement();
    TraceApertureSummary();
    lock.lock();
    if (!aperturesCleaned)
    {
        const RetireOutcome outcome = state.m_retireOutcome;
        lock.unlock();
        return outcome;
    }
    if (state.m_ownership != HostOwnership::VerifiedExclusive)
    {
        state.m_retireOutcome = RetireOutcome::OwnershipUnprovenAndPinned;
        return state.m_retireOutcome;
    }
    if (state.m_deviceHost == nullptr || state.m_teardownAttempted)
    {
        state.m_retireOutcome = state.m_teardownSucceeded ? RetireOutcome::AlreadyRetired : RetireOutcome::TeardownFailedAndPinned;
        return state.m_retireOutcome;
    }
    state.m_teardownAttempted = true;
    HDV_HOST host = state.m_deviceHost;
    lock.unlock();

    HRESULT hr = E_FAIL;
    try
    {
        hr = LoadApi().m_teardown(host);
    }
    catch (...)
    {
        hr = E_FAIL;
    }
    state.m_lastHostError.store(hr, std::memory_order_relaxed);
    (void)FAILED_LOG(hr);
    lock.lock();
    state.m_deviceHost = SUCCEEDED(hr) ? nullptr : host;
    state.m_teardownSucceeded = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        state.m_callbacksDisarmed = true;
        state.m_lifecycleChanged.wait(lock, [&state] { return state.m_activeOperations == 0; });
        state.m_deviceHandle = nullptr;
        state.m_retireOutcome = RetireOutcome::TeardownSucceeded;
        lock.unlock();
        BackendState::Unpin(&state);
        return RetireOutcome::TeardownSucceeded;
    }
    state.m_retireOutcome = RetireOutcome::TeardownFailedAndPinned;
    lock.unlock(); // no automatic retry against a partial host teardown
    return RetireOutcome::TeardownFailedAndPinned;
}

int HdvPciDeviceHost::PumpCapture(int frameBudget) noexcept
{
    if (!m_state)
    {
        return 0;
    }
    BackendState::Operation operation(m_state.get(), true);
    if (!operation)
    {
        return 0;
    }
    try
    {
        BackendState::ApertureBatch apertureBatch(*m_state);
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
        NotifyAperturePumpTestHook(m_state.get());
#endif
        const int capturedFrames = m_state->m_device->PumpCapture(frameBudget);
        return apertureBatch.Finish() ? capturedFrames : 0;
    }
    catch (...)
    {
        m_state->m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
        return 0;
    }
}

int HdvPciDeviceHost::PumpCaptureElapsed(std::uint64_t elapsedNanoseconds) noexcept
{
    if (!m_state)
    {
        return 0;
    }
    BackendState::Operation operation(m_state.get(), true);
    if (!operation)
    {
        return 0;
    }
    try
    {
        BackendState::ApertureBatch apertureBatch(*m_state);
#if defined(WSL_VIRTIO_SOUND_ADAPTER_TESTING)
        NotifyAperturePumpTestHook(m_state.get());
#endif
        const int capturedFrames = m_state->m_device->PumpCaptureElapsed(elapsedNanoseconds);
        return apertureBatch.Finish() ? capturedFrames : 0;
    }
    catch (...)
    {
        m_state->m_lastHostError.store(E_FAIL, std::memory_order_relaxed);
        return 0;
    }
}

bool HdvPciDeviceHost::CaptureRunning() const noexcept
{
    return m_state && m_state->m_device->CaptureRunning();
}

std::uint32_t HdvPciDeviceHost::CapturePeriodFrames() const noexcept
{
    return m_state ? m_state->m_device->CapturePeriodFrames() : 0;
}

std::uint32_t HdvPciDeviceHost::MaximumCaptureFrameBudget() const noexcept
{
    return m_state ? m_state->m_device->MaximumCaptureFrameBudget() : 0;
}

std::uint64_t HdvPciDeviceHost::CapturePacingGeneration() const noexcept
{
    return m_state ? m_state->m_device->CapturePacingGeneration() : 0;
}

HostOwnership HdvPciDeviceHost::Ownership() const noexcept
{
    return m_state ? m_state->m_ownership : HostOwnership::UnknownOrShared;
}

HRESULT HdvPciDeviceHost::CreationResult() const noexcept
{
    return m_state ? m_state->m_creationResult.load(std::memory_order_relaxed) : E_HANDLE;
}

HRESULT HdvPciDeviceHost::LastHostError() const noexcept
{
    return m_state ? m_state->m_lastHostError.load(std::memory_order_relaxed) : E_HANDLE;
}

VirtioSoundDevice& HdvPciDeviceHost::Device() noexcept
{
    return *m_state->m_device;
}

} // namespace wsl::virtio_sound::native::windows
