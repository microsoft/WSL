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

    TEST_METHOD(PortIds)
    {
        const usb::PortId port{L"2.1"};
        VERIFY_IS_TRUE((port.Parts == std::vector<uint32_t>{2, 1}));
        VERIFY_ARE_EQUAL(std::wstring(L"2.1"), port.ToString());
        const usb::PortId nested{L"2.1.3.4"};
        VERIFY_IS_TRUE((nested.Parts == std::vector<uint32_t>{2, 1, 3, 4}));
        VERIFY_ARE_EQUAL(std::wstring(L"2.1.3.4"), nested.ToString());
        VERIFY_IS_TRUE(nested == usb::PortId{nested.ToString()});
        VERIFY_IS_TRUE(usb::PortId{L"02.01.003.004"} == nested);
        VERIFY_IS_FALSE(port == nested);
        VERIFY_ARE_EQUAL(999u, usb::PortId{L"1.999"}.Parts.back());
        VERIFY_ARE_EQUAL(1000u, usb::PortId{L"1.1000"}.Parts.back());
        VERIFY_ARE_EQUAL(4294967295u, usb::PortId{L"1.4294967295"}.Parts.back());
        VERIFY_ARE_EQUAL(4294967295u, usb::PortId{L"4294967295.1"}.Parts.front());
        VERIFY_ARE_EQUAL(std::wstring(L"uxd_port_001"), usb::GetUxdPortRegistryValueName(1));
        VERIFY_THROWS(usb::GetUxdPortRegistryValueName(1000), wil::ResultException);
        for (const auto* invalid :
             {L"",
              L"2",
              L"2-1",
              L"2:1",
              L"0.1",
              L"1.0",
              L"-1.2",
              L"1..2",
              L"1.2 ",
              L"4294967296.1",
              L".1",
              L"1.",
              L"1.2.",
              L"1.-2",
              L"1.2-3",
              L"1.2.0",
              L"1.4294967296"})
        {
            VERIFY_THROWS(usb::PortId{invalid}, wil::ResultException);
        }
    }

    TEST_METHOD(HubPathTracksTopology)
    {
        usb::Inventory inventory(
            {{L"root-a", L"", L"", 0}, {L"root-b", L"", L"", 0}, {L"external", L"", L"root-a", 1}, {L"nested", L"", L"external", 2}});
        usb::Device device;
        device.Port = 3;
        device.HubInstanceId = L"root-a";
        const auto rootA = inventory.GetPortId(device).Parts.front();
        device.HubInstanceId = L"root-b";
        const auto rootB = inventory.GetPortId(device).Parts.front();
        device.HubInstanceId = L"nested";
        const auto original = inventory.GetPortId(device);
        VERIFY_ARE_EQUAL(std::format(L"{}.1.2.3", rootA), original.ToString());
        VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(original).InstanceId);

        auto path = inventory.GetHubPath(L"NESTED");
        VERIFY_ARE_EQUAL(size_t{3}, path.size());
        VERIFY_ARE_EQUAL(std::wstring(L"root-a"), path[0]->InstanceId);
        VERIFY_ARE_EQUAL(1u, path[1]->Port);
        VERIFY_ARE_EQUAL(2u, path[2]->Port);

        inventory.Hubs[2].Port = 1000;
        path = inventory.GetHubPath(L"nested");
        VERIFY_ARE_EQUAL(1000u, path[1]->Port);
        VERIFY_ARE_EQUAL(2u, path[2]->Port);
        const auto moved = inventory.GetPortId(device);
        VERIFY_ARE_EQUAL(std::format(L"{}.1000.2.3", rootA), moved.ToString());
        VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(moved).InstanceId);
        VERIFY_THROWS(inventory.ResolveParentHub(original), wil::ResultException);

        inventory.Hubs[2].ParentInstanceId = L"root-b";
        path = inventory.GetHubPath(L"nested");
        VERIFY_ARE_EQUAL(std::wstring(L"root-b"), path[0]->InstanceId);
        VERIFY_ARE_EQUAL(std::wstring(L"external"), path[1]->InstanceId);
        const auto movedRoot = inventory.GetPortId(device);
        VERIFY_ARE_EQUAL(std::format(L"{}.1000.2.3", rootB), movedRoot.ToString());
        VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(movedRoot).InstanceId);
        VERIFY_THROWS(inventory.ResolveParentHub(moved), wil::ResultException);

        inventory.Hubs[2].InstanceId = L"external-new-instance";
        inventory.Hubs[3].ParentInstanceId = L"external-new-instance";
        VERIFY_IS_TRUE(inventory.GetPortId(device) == movedRoot);
        VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(movedRoot).InstanceId);

        path = inventory.GetHubPath(L"root-a");
        VERIFY_ARE_EQUAL(size_t{1}, path.size());
        VERIFY_ARE_EQUAL(std::wstring(L"root-a"), path[0]->InstanceId);
        device.HubInstanceId = L"root-a";
        device.Port = 1000;
        const auto direct = inventory.GetPortId(device);
        VERIFY_ARE_EQUAL(std::format(L"{}.1000", rootA), direct.ToString());
        VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(direct).InstanceId);
        VERIFY_THROWS(inventory.ResolveParentHub(usb::PortId{L"4294967295.1"}), wil::ResultException);
        VERIFY_THROWS(inventory.ResolveParentHub(usb::PortId{}), wil::ResultException);
        usb::PortId incomplete;
        incomplete.Parts = {rootA};
        VERIFY_THROWS(inventory.ResolveParentHub(incomplete), wil::ResultException);

        VERIFY_THROWS(inventory.GetHubPath(L"missing"), wil::ResultException);
        inventory.Hubs[2].ParentInstanceId = L"missing";
        VERIFY_THROWS(inventory.GetHubPath(L"nested"), wil::ResultException);
        inventory.Hubs[2].ParentInstanceId = L"nested";
        VERIFY_THROWS(inventory.GetHubPath(L"nested"), wil::ResultException);
    }

    TEST_METHOD(RootHubNumbersAreStable)
    {
        const usb::Inventory initial({{L"stable-root-b", L"", L"", 0}, {L"stable-root-a", L"", L"", 0}});
        usb::Device device;
        device.Port = 1;
        device.HubInstanceId = L"stable-root-a";
        const auto rootA = initial.GetPortId(device);
        device.HubInstanceId = L"stable-root-b";
        const auto rootB = initial.GetPortId(device);
        VERIFY_IS_TRUE(rootA.Parts.front() < rootB.Parts.front());

        const usb::Inventory changed({{L"stable-root-c", L"", L"", 0}, {L"stable-root-b", L"", L"", 0}});
        VERIFY_IS_TRUE(rootB == changed.GetPortId(device));
        VERIFY_THROWS(changed.ResolveParentHub(rootA), wil::ResultException);
        device.HubInstanceId = L"stable-root-c";
        const auto rootC = changed.GetPortId(device);
        VERIFY_IS_TRUE(rootC.Parts.front() > rootB.Parts.front());
        VERIFY_THROWS(initial.ResolveParentHub(rootC), wil::ResultException);

        const usb::Inventory restored(
            {{L"stable-root-c", L"", L"", 0}, {L"STABLE-ROOT-A", L"", L"", 0}, {L"stable-root-b", L"", L"", 0}});
        VERIFY_IS_TRUE(rootC == restored.GetPortId(device));
        device.HubInstanceId = L"stable-root-a";
        VERIFY_IS_TRUE(rootA == restored.GetPortId(device));
        VERIFY_ARE_EQUAL(std::wstring(L"STABLE-ROOT-A"), restored.ResolveParentHub(rootA).InstanceId);
        VERIFY_IS_TRUE(rootA == initial.GetPortId(device));
        device.HubInstanceId = L"stable-root-b";
        VERIFY_IS_TRUE(rootB == restored.GetPortId(device));
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
            VERIFY_IS_TRUE(device.Port > 0);
            const auto port = inventory.GetPortId(device);
            VERIFY_IS_TRUE(port == usb::PortId{port.ToString()});
            VERIFY_ARE_EQUAL(device.HubInstanceId, inventory.ResolveParentHub(port).InstanceId);
            VERIFY_ARE_EQUAL(device.Port, port.Parts.back());
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