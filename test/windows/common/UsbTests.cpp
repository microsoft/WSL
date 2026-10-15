// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "UsbDevice.h"
#include "UsbTypes.h"
#include "UsbUxdRegistry.h"
#include "registry.hpp"
#include "stringshared.h"
#include <cfgmgr32.h>

using namespace wsl::windows::common;

namespace UsbTests {

class UsbTests
{
    WSL_TEST_CLASS(UsbTests)

    TEST_METHOD(PortSelectors)
    {
        const auto port = usb::ParsePortSelector(L"2-1");
        VERIFY_ARE_EQUAL(2u, port.Hub);
        VERIFY_ARE_EQUAL(1u, port.Port);
        VERIFY_ARE_EQUAL(999u, usb::ParsePortSelector(L"1-999").Port);
        VERIFY_ARE_EQUAL(std::wstring(L"uxd_port_001"), usb::GetUxdPortRegistryValueName(1));
        for (const auto* invalid : {L"", L"2", L"2:1", L"0-1", L"1-0", L"1-1000", L"-1-2", L"1-2-3", L"1-2 ", L"4294967296-1"})
        {
            VERIFY_THROWS(usb::ParsePortSelector(invalid), wil::ResultException);
        }
    }

    TEST_METHOD(ReservationLayoutAndOwnership)
    {
        const GUID owner{0x01234567, 0x89ab, 0x4cde, {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
        auto reservation = usb::MakeReservation(owner);
        const auto bytes = std::span(reinterpret_cast<const BYTE*>(&reservation), sizeof(reservation));
        VERIFY_ARE_EQUAL(size_t{68}, bytes.size());
        VERIFY_ARE_EQUAL(BYTE{1}, bytes[36]);
        VERIFY_ARE_EQUAL(BYTE{1}, bytes[40]);
        VERIFY_ARE_EQUAL(BYTE{1}, bytes[44]);
        VERIFY_IS_TRUE(std::ranges::all_of(bytes.subspan(48), [](BYTE value) { return value == 0; }));
        VERIFY_IS_TRUE(usb::OwnsReservation(owner, usb::DecodeReservation(bytes)));
        VERIFY_THROWS(usb::DecodeReservation(bytes.first(67)), wil::ResultException);
        const BYTE oversized[69]{};
        VERIFY_THROWS(usb::DecodeReservation(oversized), wil::ResultException);
        reservation.Version = 2;
        VERIFY_THROWS(usb::DecodeReservation(bytes), wil::ResultException);
        reservation = usb::MakeReservation(owner);
        ++reservation.OwnerGuid.Data1;
        VERIFY_IS_FALSE(usb::OwnsReservation(owner, reservation));
        reservation = usb::MakeReservation(owner);
        ++reservation.PnpGuid.Data1;
        VERIFY_IS_FALSE(usb::OwnsReservation(owner, reservation));
        reservation = usb::MakeReservation(owner);
        reservation.Reserved[0] = 1;
        VERIFY_IS_FALSE(usb::OwnsReservation(owner, reservation));
    }

    TEST_METHOD(MappingMatchesRejectsNonmatchingValues)
    {
        usb::UxdReservationIdentity identity;
        VERIFY_SUCCEEDED(CoCreateGuid(&identity.Id));
        const auto name = wsl::shared::string::GuidToString<wchar_t>(identity.Id);
        const auto key = registry::CreateKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\UsbHub\\uxd_control\\pnp");
        usb::UxdRegistry uxdRegistry;
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));

        registry::WriteString(key.get(), nullptr, name.c_str(), usb::c_winusbHardwareId);
        auto cleanup = wil::scope_exit([&]() { LOG_IF_WIN32_ERROR(RegDeleteValueW(key.get(), name.c_str())); });
        VERIFY_IS_TRUE(uxdRegistry.MappingMatches(identity));
        registry::WriteString(key.get(), nullptr, name.c_str(), L"usb\\ms_comp_winusb");
        VERIFY_IS_TRUE(uxdRegistry.MappingMatches(identity));

        for (const auto* nonmatching : {L"USB\\MS_COMP_WINUSX", L"USB\\OTHER", L"USB\\MS_COMP_WINUSB_EXTRA"})
        {
            registry::WriteString(key.get(), nullptr, name.c_str(), nonmatching);
            VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));
        }
        uxdRegistry.RemoveMapping(identity);
        VERIFY_ARE_EQUAL(std::wstring(L"USB\\MS_COMP_WINUSB_EXTRA"), registry::ReadString(key.get(), nullptr, name.c_str()));

        VERIFY_ARE_EQUAL(
            ERROR_SUCCESS,
            RegSetValueExW(key.get(), name.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(usb::c_winusbHardwareId), sizeof(usb::c_winusbHardwareId)));
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));

        wchar_t unterminated[] = L"USB\\MS_COMP_WINUSB";
        unterminated[ARRAYSIZE(unterminated) - 1] = L'X';
        VERIFY_ARE_EQUAL(
            ERROR_SUCCESS,
            RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(unterminated), sizeof(unterminated)));
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));
        VERIFY_ARE_EQUAL(ERROR_SUCCESS, RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ, nullptr, 0));
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));

        VERIFY_ARE_EQUAL(ERROR_SUCCESS, RegDeleteValueW(key.get(), name.c_str()));
        cleanup.release();
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));
    }

    TEST_METHOD(MappingReferencesIgnoreMalformedReservations)
    {
        usb::UxdReservationIdentity identity;
        VERIFY_SUCCEEDED(CoCreateGuid(&identity.Id));
        const auto name = wsl::shared::string::GuidToString<wchar_t>(identity.Id);
        const auto hardwarePath = L"SYSTEM\\CurrentControlSet\\Enum\\USB\\WSL_USB_TEST_" + name;
        const auto hardwareKey = registry::CreateKey(HKEY_LOCAL_MACHINE, hardwarePath.c_str());
        auto cleanupHardware =
            wil::scope_exit([&]() { LOG_IF_WIN32_ERROR(RegDeleteTreeW(HKEY_LOCAL_MACHINE, hardwarePath.c_str())); });
        const auto reservationKey = registry::CreateKey(hardwareKey.get(), L"0\\Device Parameters\\uxd");
        usb::UxdRegistry uxdRegistry;

        registry::WriteDword(reservationKey.get(), nullptr, L"uxd_port_001", 1);
        VERIFY_IS_FALSE(uxdRegistry.MappingReferenced(identity));

        const BYTE invalidBytes[sizeof(usb::Reservation) + 1]{};
        for (const DWORD size : {0u, 1u, 67u, 69u})
        {
            VERIFY_ARE_EQUAL(ERROR_SUCCESS, RegSetValueExW(reservationKey.get(), L"uxd_port_001", 0, REG_BINARY, invalidBytes, size));
            VERIFY_IS_FALSE(uxdRegistry.MappingReferenced(identity));
        }

        auto reservation = usb::MakeReservation(identity.Id);
        reservation.Version = 2;
        VERIFY_ARE_EQUAL(
            ERROR_SUCCESS,
            RegSetValueExW(reservationKey.get(), L"uxd_port_001", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&reservation), sizeof(reservation)));
        VERIFY_IS_FALSE(uxdRegistry.MappingReferenced(identity));

        const auto mappingKey =
            registry::CreateKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\UsbHub\\uxd_control\\pnp");
        registry::WriteString(mappingKey.get(), nullptr, name.c_str(), usb::c_winusbHardwareId);
        auto cleanupMapping = wil::scope_exit([&]() { LOG_IF_WIN32_ERROR(RegDeleteValueW(mappingKey.get(), name.c_str())); });

        // A valid reference must still prevent deletion of its mapping.
        reservation.Version = 1;
        VERIFY_ARE_EQUAL(
            ERROR_SUCCESS,
            RegSetValueExW(reservationKey.get(), L"uxd_port_001", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&reservation), sizeof(reservation)));
        VERIFY_IS_TRUE(uxdRegistry.MappingReferenced(identity));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BUSY), wil::ResultFromException([&]() { uxdRegistry.RemoveMapping(identity); }));
        VERIFY_IS_TRUE(uxdRegistry.MappingMatches(identity));

        registry::WriteDword(reservationKey.get(), nullptr, L"uxd_port_001", 1);
        uxdRegistry.RemoveMapping(identity);
        cleanupMapping.release();
        VERIFY_IS_FALSE(uxdRegistry.MappingMatches(identity));
        VERIFY_ARE_EQUAL(1u, registry::ReadDword(reservationKey.get(), nullptr, L"uxd_port_001", 0));
    }

    TEST_METHOD(WinUsbReadiness)
    {
        usb::Device device;
        device.HardwareIds = {L"USB\\MS_COMP_WINUSB"};
        device.Service = L"WINUSB";
        device.Status = DN_STARTED;
        device.InterfacePaths = {L"opaque-device-interface"};
        VERIFY_IS_TRUE(device.IsReady());
        device.Status |= DN_HAS_PROBLEM;
        VERIFY_IS_FALSE(device.IsReady());
        device.Status = DN_STARTED;
        device.Service = L"usbser";
        VERIFY_IS_FALSE(device.IsReady());
        device.Service = L"WINUSB";
        device.InterfacePaths.clear();
        VERIFY_IS_FALSE(device.IsReady());
    }

    TEST_METHOD(WinUsbProbeErrors)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto path = std::filesystem::temp_directory_path() / (L"wsl-usb-probe-" + wsl::shared::string::GuidToString<wchar_t>(id));
        usb::Device device;
        device.HardwareIds = {L"USB\\MS_COMP_WINUSB"};
        device.Service = L"WINUSB";
        device.Status = DN_STARTED;
        device.InterfacePaths = {path.wstring()};
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), wil::ResultFromException([&]() { device.ProbeWinUsb(); }));

        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        VERIFY_IS_TRUE(static_cast<bool>(file));
        auto cleanup = wil::scope_exit([&]() { DeleteFileW(path.c_str()); });
        file.reset();
        VERIFY_THROWS(device.ProbeWinUsb(), wil::ResultException);

        device.Status = 0;
        VERIFY_IS_FALSE(device.ProbeWinUsb().has_value());
    }

    TEST_METHOD(PhysicalDeviceInventory)
    {
        const auto inventory = usb::Inventory::Enumerate();
        for (const auto& device : inventory.Devices)
        {
            VERIFY_IS_TRUE(device.Port > 0 && device.Port <= 999);
            VERIFY_IS_TRUE(std::ranges::any_of(inventory.Hubs, [&](const auto& hub) {
                return wsl::shared::string::IsEqual(hub.InstanceId, device.HubInstanceId, true);
            }));
            VERIFY_IS_FALSE(std::ranges::any_of(inventory.Hubs, [&](const auto& hub) {
                return wsl::shared::string::IsEqual(hub.InstanceId, device.InstanceId, true);
            }));
        }
    }
};

} // namespace UsbTests