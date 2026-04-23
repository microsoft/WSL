/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IWSLCVolume.h

Abstract:

    Abstract interface implemented by all WSLC-managed volume drivers
    (currently WSLCVhdVolumeImpl and WSLCGuestVolumeImpl). WSLCSession
    stores volumes through this interface so it can hold a single map
    regardless of the concrete driver type.

--*/

#pragma once

#include "wslc.h"
#include <string>

namespace wsl::windows::service::wslc {

class IWSLCVolume
{
public:
    virtual ~IWSLCVolume() = default;

    // The docker volume name.
    virtual const std::string& Name() const noexcept = 0;

    // The WSLC volume driver, e.g. "vhd" or "guest". This is the driver
    // stored in the WSLC volume metadata label, not the underlying docker
    // driver (which may be "local" for guest volumes).
    virtual const char* Driver() const noexcept = 0;

    // Remove the volume from docker and release any host-side resources
    // (e.g. detach/delete the VHD for VHD volumes). Throws on failure.
    virtual void Delete() = 0;

    // Returns a JSON string for the COM-facing InspectVolume result.
    virtual std::string Inspect() const = 0;

    // Returns the WSLCVolumeInformation struct for the COM-facing
    // ListVolumes / CreateVolume results.
    virtual WSLCVolumeInformation GetVolumeInformation() const = 0;
};

} // namespace wsl::windows::service::wslc
