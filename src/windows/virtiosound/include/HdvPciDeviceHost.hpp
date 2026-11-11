// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <hypervdevicevirtualization.h>
#include <windows.h>

#include <memory>

#include "VirtioSoundDevice.hpp"

namespace wsl::virtio_sound::native::windows {

// Direct HDV initialization against a borrowed WSL HCS_SYSTEM is not publicly
// documented to return an independent host when HdvProxyDeviceHost is already
// registered. The default is therefore UnknownOrShared. A caller may specify
// VerifiedExclusive only after establishing that the HDV host is exclusively
// owned and can be torn down without affecting another registration.
enum class HostOwnership
{
    UnknownOrShared,
    VerifiedExclusive
};

enum class RetireOutcome
{
    OwnershipUnprovenAndPinned,
    TeardownSucceeded,
    TeardownFailedAndPinned,
    AlreadyRetired
};

// Registers one PCI virtio-sound function against exactly the supplied,
// borrowed HCS_SYSTEM. It never opens or closes that handle and never performs
// VM-id lookup. Null source selects the deterministic synthetic ramp.
class HdvPciDeviceHost final
{
public:
    static constexpr HRESULT c_ownershipUnprovenHresult = static_cast<HRESULT>(0x8004A001L);

    static std::unique_ptr<HdvPciDeviceHost> CreateBorrowed(
        HCS_SYSTEM borrowedSystem,
        const GUID& deviceClassId,
        const GUID& deviceInstanceId,
        std::shared_ptr<PcmSource> source = {},
        HostOwnership ownership = HostOwnership::UnknownOrShared);

    ~HdvPciDeviceHost();
    HdvPciDeviceHost(const HdvPciDeviceHost&) = delete;
    HdvPciDeviceHost& operator=(const HdvPciDeviceHost&) = delete;

    // Called by the owning WSL service after its VM has stopped. This disables
    // pumping. UnknownOrShared retains the callback root for process lifetime
    // and never calls HdvTeardownDeviceHost. VerifiedExclusive attempts
    // teardown once; failure also retains the callback root and is not retried.
    RetireOutcome GuestStoppedAndRetire() noexcept;
    int PumpCapture(int frameBudget) noexcept;
    int PumpCaptureElapsed(std::uint64_t elapsedNanoseconds) noexcept;
    bool CaptureRunning() const noexcept;
    std::uint32_t CapturePeriodFrames() const noexcept;
    std::uint32_t MaximumCaptureFrameBudget() const noexcept;
    std::uint64_t CapturePacingGeneration() const noexcept;

    HostOwnership Ownership() const noexcept;
    HRESULT CreationResult() const noexcept;
    HRESULT LastHostError() const noexcept;
    VirtioSoundDevice& Device() noexcept;

private:
    class BackendState;
    explicit HdvPciDeviceHost(std::shared_ptr<BackendState> state) noexcept;
    std::shared_ptr<BackendState> m_state;
};

} // namespace wsl::virtio_sound::native::windows
