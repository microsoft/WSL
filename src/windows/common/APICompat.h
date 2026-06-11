/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    APICompat.h

Abstract:

    API compatibility layer between the wslc.idl and wslccompat.idl.

--*/

#pragma once

#include <vector>
#include <wrl/client.h>
#include "wslc.h"
#include "WSLCCompat.h"

namespace wsl::windows::common::apicompat {

//
// Enum conversions.
//

// Forward (WSLCCompat -> wslc.idl).
WSLCFD Convert(WSLCCompatFD Fd);
WSLCSignal Convert(WSLCCompatSignal Signal);
WSLCProcessFlags Convert(WSLCCompatProcessFlags Flags);
WSLCContainerFlags Convert(WSLCCompatContainerFlags Flags);
WSLCContainerStartFlags Convert(WSLCCompatContainerStartFlags Flags);
WSLCDeleteFlags Convert(WSLCCompatDeleteFlags Flags);
WSLCNetworkingMode Convert(WSLCCompatNetworkingMode Mode);
WSLCFeatureFlags Convert(WSLCCompatFeatureFlags Flags);
WSLCSessionStorageFlags Convert(WSLCCompatSessionStorageFlags Flags);
WSLCSessionFlags Convert(WSLCCompatSessionFlags Flags);
WSLCHandleType Convert(WSLCCompatHandleType Type);

// Reverse (wslc.idl -> WSLCCompat).
WSLCCompatProcessState Convert(WSLCProcessState State);
WSLCCompatContainerState Convert(WSLCContainerState State);
WSLCCompatVirtualMachineTerminationReason Convert(WSLCVirtualMachineTerminationReason Reason);
WSLCCompatHandleType Convert(WSLCHandleType Type);
WSLCCompatDeletedImageType Convert(WSLCDeletedImageType Type);

// Forward (WSLCCompat -> wslc.idl).
WSLCVersion Convert(const WSLCCompatVersion& Version);
WSLCHandle Convert(const WSLCCompatHandle& Handle);
WSLCStringArray Convert(const WSLCCompatStringArray& Array);
WSLCProcessOptions Convert(const WSLCCompatProcessOptions& Options);
KeyValuePair Convert(const WSLCCompatKeyValuePair& Pair);
WSLCVolume Convert(const WSLCCompatVolume& Volume);
WSLCNamedVolume Convert(const WSLCCompatNamedVolume& Volume);
WSLCPortMapping Convert(const WSLCCompatPortMapping& Port);
WSLCTmpfsMount Convert(const WSLCCompatTmpfsMount& Mount);
WSLCUlimit Convert(const WSLCCompatUlimit& Ulimit);
WSLCDeleteImageOptions Convert(const WSLCCompatDeleteImageOptions& Options);
WSLCTagImageOptions Convert(const WSLCCompatTagImageOptions& Options);

// Reverse (wslc.idl -> WSLCCompat).
WSLCCompatVersion Convert(const WSLCVersion& Version);
WSLCCompatHandle Convert(const WSLCHandle& Handle);
WSLCCompatImageInformation Convert(const WSLCImageInformation& Image);
WSLCCompatDeletedImageInformation Convert(const WSLCDeletedImageInformation& Image);
WSLCCompatVolumeInformation Convert(const WSLCVolumeInformation& Volume);

//
// Composite struct conversions. These contain nested arrays whose element types
// differ between the two contracts, so the converted struct owns backing
// storage that must outlive the forwarded call. Construct the conversion as a
// local and pass Get() to the wslc.idl method.
//

class ContainerOptionsConversion
{
public:
    NON_COPYABLE(ContainerOptionsConversion);
    DEFAULT_MOVABLE(ContainerOptionsConversion);

    explicit ContainerOptionsConversion(const WSLCCompatContainerOptions& Options);

    const WSLCContainerOptions* Get() const noexcept
    {
        return &m_value;
    }

private:
    WSLCContainerOptions m_value{};
    std::vector<WSLCVolume> m_volumes;
    std::vector<WSLCPortMapping> m_ports;
    std::vector<WSLCLabel> m_labels;
    std::vector<WSLCTmpfsMount> m_tmpfs;
    std::vector<WSLCNamedVolume> m_namedVolumes;
    std::vector<WSLCUlimit> m_ulimits;
    std::vector<WSLCNetworkConnection> m_networks;
    std::vector<std::vector<KeyValuePair>> m_networkSettings;
};

class ListImagesOptionsConversion
{
public:
    NON_COPYABLE(ListImagesOptionsConversion);
    DEFAULT_MOVABLE(ListImagesOptionsConversion);

    explicit ListImagesOptionsConversion(const WSLCCompatListImagesOptions& Options);

    const WSLCListImagesOptions* Get() const noexcept
    {
        return &m_value;
    }

private:
    WSLCListImagesOptions m_value{};
    std::vector<WSLCFilter> m_filters;
};

class VolumeOptionsConversion
{
public:
    NON_COPYABLE(VolumeOptionsConversion);
    DEFAULT_MOVABLE(VolumeOptionsConversion);

    explicit VolumeOptionsConversion(const WSLCCompatVolumeOptions& Options);

    const WSLCVolumeOptions* Get() const noexcept
    {
        return &m_value;
    }

private:
    WSLCVolumeOptions m_value{};
    std::vector<WSLCDriverOption> m_driverOpts;
    std::vector<WSLCLabel> m_labels;
};

class SessionSettingsConversion
{
public:
    NON_COPYABLE(SessionSettingsConversion);
    DEFAULT_MOVABLE(SessionSettingsConversion);

    explicit SessionSettingsConversion(const WSLCCompatSessionSettings& Settings);

    const WSLCSessionSettings* Get() const noexcept
    {
        return &m_value;
    }

private:
    WSLCSessionSettings m_value{};
    Microsoft::WRL::ComPtr<ITerminationCallback> m_terminationCallback;
};

inline ContainerOptionsConversion Convert(const WSLCCompatContainerOptions& Options)
{
    return ContainerOptionsConversion(Options);
}

inline ListImagesOptionsConversion Convert(const WSLCCompatListImagesOptions& Options)
{
    return ListImagesOptionsConversion(Options);
}

inline VolumeOptionsConversion Convert(const WSLCCompatVolumeOptions& Options)
{
    return VolumeOptionsConversion(Options);
}

inline SessionSettingsConversion Convert(const WSLCCompatSessionSettings& Settings)
{
    return SessionSettingsConversion(Settings);
}

//
// Callback conversions.

Microsoft::WRL::ComPtr<ITerminationCallback> Convert(IWSLCCompatTerminationCallback* Callback);
Microsoft::WRL::ComPtr<ICrashDumpCallback> Convert(IWSLCCompatCrashDumpCallback* Callback);
Microsoft::WRL::ComPtr<IProgressCallback> Convert(IWSLCCompatProgressCallback* Callback);
Microsoft::WRL::ComPtr<IWarningCallback> Convert(IWSLCCompatWarningCallback* Callback);

} // namespace wsl::windows::common::apicompat
