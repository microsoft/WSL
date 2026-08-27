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
#include <map>
#include <string>
#include <utility>

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

    // The user-specified labels on this volume (excludes the WSLC metadata label).
    virtual const std::map<std::string, std::string>& Labels() const noexcept = 0;

    // The status of the volume as {Code, Message}: S_OK with an empty message when the volume
    // opened successfully and is usable, otherwise a failure HRESULT and a human-readable reason
    // (e.g. the backing VHD is missing).
    virtual std::pair<HRESULT, std::string> Status() const
    {
        return {S_OK, {}};
    }

    // Remove the volume from docker and release any host-side resources
    // (e.g. detach/delete the VHD for VHD volumes). Throws on failure.
    virtual void Delete() = 0;

    // Called when Docker has already destroyed the volume (e.g. container delete with -v).
    // Releases any host-side resources without contacting Docker. Default is a no-op.
    virtual void OnDeleted()
    {
    }

    // Returns a JSON string for the COM-facing InspectVolume result.
    virtual std::string Inspect() const = 0;

    // Returns the WSLCVolumeInformation struct for the COM-facing
    // ListVolumes / CreateVolume results.
    virtual WSLCVolumeInformation GetVolumeInformation() const = 0;
};

} // namespace wsl::windows::service::wslc
