// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windows.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wsl::windows::common::usb {

inline constexpr wchar_t c_winusbHardwareId[] = L"USB\\MS_COMP_VMREDIR";

#pragma pack(push, 4)
struct Reservation
{
    uint32_t Version;
    GUID PnpGuid;
    GUID OwnerGuid;
    uint32_t DeleteOnShutdown;
    uint32_t DeleteOnReload;
    uint32_t DeleteOnDisconnect;
    uint32_t Reserved[5];
};
#pragma pack(pop)

struct UxdReservationIdentity
{
    std::wstring HubInstanceId;
    uint32_t Port = 0;
    GUID Id{};
};

namespace uxd {

    std::wstring GetUxdPortRegistryValueName(uint32_t Port);
    Reservation MakeReservation(const GUID& Owner);
    Reservation DecodeReservation(std::span<const BYTE> Bytes);
    bool OwnsReservation(const GUID& Owner, const Reservation& Value) noexcept;

    bool HasReservation(const UxdReservationIdentity& Value);
    std::optional<Reservation> ReadReservation(const UxdReservationIdentity& Value);
    void CreateMapping(const UxdReservationIdentity& Value);
    // Missing or nonmatching values return false; registry access errors still throw.
    bool MappingMatches(const UxdReservationIdentity& Value);
    void PublishReservation(const UxdReservationIdentity& Value);
    bool RemoveReservation(const UxdReservationIdentity& Value);
    bool MappingReferenced(const UxdReservationIdentity& Value);
    void RemoveMapping(const UxdReservationIdentity& Value);
    void Flush();
    bool IsSupported();

} // namespace uxd

} // namespace wsl::windows::common::usb