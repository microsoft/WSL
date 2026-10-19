// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "UsbUxdRegistry.h"
#include "stringshared.h"
#include <cfgmgr32.h>
#include <cstddef>
#include <ktmw32.h>
#include <setupapi.h>
#include <type_traits>

namespace wsl::windows::common::usb {

namespace {

    constexpr wchar_t c_policyPath[] = L"SYSTEM\\CurrentControlSet\\Services\\UsbHub\\uxd_control\\policy";
    constexpr wchar_t c_mappingPath[] = L"SYSTEM\\CurrentControlSet\\Services\\UsbHub\\uxd_control\\pnp";
    constexpr wchar_t c_enumPath[] = L"SYSTEM\\CurrentControlSet\\Enum\\";

    wil::unique_hkey OpenRegistryKeyIfExists(HKEY Root, const std::wstring& Path, REGSAM Access = KEY_READ)
    {
        auto [key, result] = registry::OpenKeyNoThrow(Root, Path.c_str(), Access);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        {
            return {};
        }
        THROW_IF_FAILED(result);
        return std::move(key);
    }

    bool RegistryValueExists(HKEY Key, const std::wstring& Name)
    {
        if (Key == nullptr)
        {
            return false;
        }

        const auto error = RegQueryValueExW(Key, Name.c_str(), nullptr, nullptr, nullptr, nullptr);
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return false;
        }
        THROW_IF_WIN32_ERROR(error);
        return true;
    }

    std::optional<Reservation> ReadReservationRegistryValue(HKEY Key, const std::wstring& Name)
    {
        if (Key == nullptr)
        {
            return {};
        }

        BYTE bytes[sizeof(Reservation)]{};
        DWORD size = sizeof(bytes);
        DWORD type = 0;
        const auto error = RegQueryValueExW(Key, Name.c_str(), nullptr, &type, bytes, &size);
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return {};
        }

        THROW_IF_WIN32_ERROR(error);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), type != REG_BINARY);
        return DecodeReservation(std::span(bytes, size));
    }

    bool IsWinUsbMapping(HKEY Key, const std::wstring& Name)
    {
        if (Key == nullptr)
        {
            return false;
        }

        // Require the mapping to contain the exact WinUSB hardware ID.
        wchar_t text[ARRAYSIZE(c_winusbHardwareId)]{};
        DWORD size = sizeof(text);
        DWORD type = 0;
        const auto error = RegQueryValueExW(Key, Name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(text), &size);
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_MORE_DATA)
        {
            return false;
        }

        THROW_IF_WIN32_ERROR(error);
        return type == REG_SZ && size == sizeof(text) && text[ARRAYSIZE(text) - 1] == L'\0' &&
               wsl::shared::string::IsEqual(text, c_winusbHardwareId, true);
    }

    std::wstring GetMappingRegistryValueName(const GUID& Value)
    {
        wchar_t text[39]{};
        THROW_HR_IF(E_INVALIDARG, StringFromGUID2(Value, text, ARRAYSIZE(text)) == 0);
        return text;
    }

    void DeleteRegistryValueIfExists(HKEY Key, const std::wstring& Name)
    {
        const auto error = RegDeleteValueW(Key, Name.c_str());
        THROW_IF_WIN32_ERROR(error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error);
    }

} // namespace

static_assert(std::is_standard_layout_v<Reservation>);
static_assert(std::is_trivially_copyable_v<Reservation>);
static_assert(alignof(Reservation) == 4);
static_assert(sizeof(Reservation) == 68);
static_assert(sizeof(Reservation::Reserved) == 20);
static_assert(offsetof(Reservation, Version) == 0);
static_assert(offsetof(Reservation, PnpGuid) == 4);
static_assert(offsetof(Reservation, OwnerGuid) == 20);
static_assert(offsetof(Reservation, DeleteOnShutdown) == 36);
static_assert(offsetof(Reservation, DeleteOnReload) == 40);
static_assert(offsetof(Reservation, DeleteOnDisconnect) == 44);
static_assert(offsetof(Reservation, Reserved) == 48);

std::wstring GetUxdPortRegistryValueName(uint32_t Port)
{
    THROW_HR_IF(E_INVALIDARG, Port == 0 || Port > 999);
    return std::format(L"uxd_port_{:03}", Port);
}

Reservation MakeReservation(const GUID& Owner)
{
    return {1, Owner, Owner, 1, 1, 1, {}};
}

Reservation DecodeReservation(std::span<const BYTE> Bytes)
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), Bytes.size() != sizeof(Reservation));
    Reservation result{};
    memcpy(&result, Bytes.data(), sizeof(result));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), result.Version != 1);
    return result;
}

bool OwnsReservation(const GUID& Owner, const Reservation& Value) noexcept
{
    return Value.Version == 1 && IsEqualGUID(Owner, Value.PnpGuid) && IsEqualGUID(Owner, Value.OwnerGuid) &&
           Value.DeleteOnShutdown == 1 && Value.DeleteOnReload == 1 && Value.DeleteOnDisconnect == 1 &&
           std::ranges::all_of(Value.Reserved, [](uint32_t value) { return value == 0; });
}

std::wstring UxdRegistry::GetReservationRegistryPath(const UxdReservationIdentity& Value) const
{
    // Validate the recorded USB instance ID before using it as a registry path.
    const auto separator = Value.HubInstanceId.find(L'\\', 4);
    THROW_HR_IF(
        E_INVALIDARG,
        !wsl::shared::string::StartsWith(Value.HubInstanceId, L"USB\\", true) || separator == std::wstring::npos || separator == 4 ||
            separator + 1 == Value.HubInstanceId.size() || Value.HubInstanceId.find(L'\\', separator + 1) != std::wstring::npos ||
            Value.HubInstanceId.size() >= MAX_DEVICE_ID_LEN);
    return c_enumPath + Value.HubInstanceId + L"\\Device Parameters\\uxd";
}

bool UxdRegistry::HasReservation(const UxdReservationIdentity& Value) const
{
    const auto key = OpenRegistryKeyIfExists(HKEY_LOCAL_MACHINE, GetReservationRegistryPath(Value));
    return RegistryValueExists(key.get(), GetUxdPortRegistryValueName(Value.Port));
}

std::optional<Reservation> UxdRegistry::ReadReservation(const UxdReservationIdentity& Value) const
{
    const auto key = OpenRegistryKeyIfExists(HKEY_LOCAL_MACHINE, GetReservationRegistryPath(Value));
    return ReadReservationRegistryValue(key.get(), GetUxdPortRegistryValueName(Value.Port));
}

void UxdRegistry::CreateMapping(const UxdReservationIdentity& Value)
{
    // Enable shared UXD support, then publish our GUID-to-WinUSB mapping.
    const auto policy = registry::CreateKey(HKEY_LOCAL_MACHINE, c_policyPath);
    if (registry::ReadDword(policy.get(), nullptr, L"UxdGlobalEnable", 0) != 1)
    {
        registry::WriteDword(policy.get(), nullptr, L"UxdGlobalEnable", 1);
    }

    const auto key = registry::CreateKey(HKEY_LOCAL_MACHINE, c_mappingPath);
    const auto name = GetMappingRegistryValueName(Value.Id);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), RegistryValueExists(key.get(), name));
    registry::WriteString(key.get(), nullptr, name.c_str(), c_winusbHardwareId);
}

bool UxdRegistry::MappingMatches(const UxdReservationIdentity& Value) const
{
    const auto key = OpenRegistryKeyIfExists(HKEY_LOCAL_MACHINE, c_mappingPath);
    return IsWinUsbMapping(key.get(), GetMappingRegistryValueName(Value.Id));
}

void UxdRegistry::PublishReservation(const UxdReservationIdentity& Value)
{
    // Check and publish the port reservation in one registry transaction.
    const auto handle = CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr);
    THROW_LAST_ERROR_IF(handle == INVALID_HANDLE_VALUE);
    wil::unique_handle transaction(handle);

    wil::unique_hkey key;
    THROW_IF_WIN32_ERROR(RegCreateKeyTransactedW(
        HKEY_LOCAL_MACHINE,
        GetReservationRegistryPath(Value).c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        key.put(),
        nullptr,
        transaction.get(),
        nullptr));

    const auto name = GetUxdPortRegistryValueName(Value.Port);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), RegistryValueExists(key.get(), name));
    const auto entry = MakeReservation(Value.Id);
    THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), name.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(&entry), sizeof(entry)));
    THROW_IF_WIN32_BOOL_FALSE(CommitTransaction(transaction.get()));
}

bool UxdRegistry::RemoveReservation(const UxdReservationIdentity& Value)
{
    // Check ownership and delete the reservation in one registry transaction.
    const auto handle = CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr);
    THROW_LAST_ERROR_IF(handle == INVALID_HANDLE_VALUE);
    wil::unique_handle transaction(handle);

    wil::unique_hkey key;
    const auto error = RegOpenKeyTransactedW(
        HKEY_LOCAL_MACHINE, GetReservationRegistryPath(Value).c_str(), 0, KEY_READ | KEY_WRITE, key.put(), transaction.get(), nullptr);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    {
        return false;
    }
    THROW_IF_WIN32_ERROR(error);

    const auto name = GetUxdPortRegistryValueName(Value.Port);
    const auto entry = ReadReservationRegistryValue(key.get(), name);
    if (!entry)
    {
        return false;
    }

    THROW_HR_IF(E_ACCESSDENIED, !OwnsReservation(Value.Id, *entry));
    DeleteRegistryValueIfExists(key.get(), name);
    THROW_IF_WIN32_BOOL_FALSE(CommitTransaction(transaction.get()));
    return true;
}

bool UxdRegistry::MappingReferenced(const UxdReservationIdentity& Value) const
{
    // Check all USB instances, including non-present devices, before allowing mapping deletion.
    const auto root = OpenRegistryKeyIfExists(HKEY_LOCAL_MACHINE, std::wstring(c_enumPath) + L"USB");
    if (root)
    {
        for (const auto& [hardwareName, hardware] : registry::EnumKeys(root.get(), KEY_READ))
        {
            for (const auto& [instanceName, instance] : registry::EnumKeys(hardware.get(), KEY_READ))
            {
                const auto key = OpenRegistryKeyIfExists(instance.get(), L"Device Parameters\\uxd");
                if (!key)
                {
                    continue;
                }

                for (const auto& [name, type] : registry::EnumValues(key.get()))
                {
                    if (type != REG_BINARY || !wsl::shared::string::StartsWith(name, L"uxd_port_", true))
                    {
                        continue;
                    }

                    try
                    {
                        const auto entry = ReadReservationRegistryValue(key.get(), name);
                        if (entry && IsEqualGUID(entry->PnpGuid, Value.Id))
                        {
                            return true;
                        }
                    }
                    catch (...)
                    {
                        // Malformed entries must not block cleanup of unrelated mappings.
                        const auto error = wil::ResultFromCaughtException();
                        THROW_HR_IF(error, error != HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && error != HRESULT_FROM_WIN32(ERROR_MORE_DATA));
                    }
                }
            }
        }
    }
    return false;
}

void UxdRegistry::RemoveMapping(const UxdReservationIdentity& Value)
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), MappingReferenced(Value));

    const auto key = OpenRegistryKeyIfExists(HKEY_LOCAL_MACHINE, c_mappingPath, KEY_READ | KEY_WRITE);
    const auto name = GetMappingRegistryValueName(Value.Id);
    if (IsWinUsbMapping(key.get(), name))
    {
        DeleteRegistryValueIfExists(key.get(), name);
    }
}

void UxdRegistry::Flush() const
{
    // Flush SYSTEM before the manager deletes ownership records from the SOFTWARE journal.
    const auto key = registry::OpenKey(HKEY_LOCAL_MACHINE, L"SYSTEM", KEY_QUERY_VALUE);
    THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
}

bool UxdRegistry::IsSupported()
{
    static const bool supported = QuerySupport() == S_OK;
    return supported;
}

HRESULT UxdRegistry::QuerySupport() noexcept
try
{
    const auto infPath = wil::GetWindowsDirectoryW<std::wstring>() + L"\\INF\\winusb.inf";
    const auto rawInf = SetupOpenInfFileW(infPath.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    THROW_LAST_ERROR_IF(rawInf == INVALID_HANDLE_VALUE);
    wil::unique_any<HINF, decltype(&SetupCloseInfFile), SetupCloseInfFile> inf(rawInf);

    const auto rawDevices = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    THROW_LAST_ERROR_IF(rawDevices == INVALID_HANDLE_VALUE);
    wil::unique_any<HDEVINFO, decltype(&SetupDiDestroyDeviceInfoList), SetupDiDestroyDeviceInfoList> devices(rawDevices);

    SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
    THROW_IF_WIN32_BOOL_FALSE(SetupDiGetDeviceInstallParamsW(devices.get(), nullptr, &parameters));
    WI_SetFlag(parameters.Flags, DI_ENUMSINGLEINF);
    WI_SetFlag(parameters.FlagsEx, DI_FLAGSEX_ALLOWEXCLUDEDDRVS);
    THROW_IF_FAILED(StringCchCopyW(parameters.DriverPath, ARRAYSIZE(parameters.DriverPath), infPath.c_str()));
    THROW_IF_WIN32_BOOL_FALSE(SetupDiSetDeviceInstallParamsW(devices.get(), nullptr, &parameters));
    THROW_IF_WIN32_BOOL_FALSE(SetupDiBuildDriverInfoList(devices.get(), nullptr, SPDIT_CLASSDRIVER));

    for (DWORD index = 0;; ++index)
    {
        SP_DRVINFO_DATA_W driver{sizeof(driver)};
        if (!SetupDiEnumDriverInfoW(devices.get(), nullptr, SPDIT_CLASSDRIVER, index, &driver))
        {
            const auto error = GetLastError();
            THROW_WIN32_IF(error, error != ERROR_NO_MORE_ITEMS);
            return S_FALSE;
        }

        DWORD size = 0;
        if (!SetupDiGetDriverInfoDetailW(devices.get(), nullptr, &driver, nullptr, 0, &size))
        {
            const auto error = GetLastError();
            THROW_WIN32_IF(error, error != ERROR_INSUFFICIENT_BUFFER);
        }
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), size < sizeof(SP_DRVINFO_DETAIL_DATA_W));
        std::vector<BYTE> buffer(size);
        auto* details = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
        details->cbSize = sizeof(*details);
        THROW_IF_WIN32_BOOL_FALSE(SetupDiGetDriverInfoDetailW(devices.get(), nullptr, &driver, details, size, nullptr));

        const auto capacity = (buffer.size() - offsetof(SP_DRVINFO_DETAIL_DATA_W, HardwareID)) / sizeof(wchar_t);
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            details->CompatIDsOffset > capacity || details->CompatIDsLength > capacity - details->CompatIDsOffset);
        const std::wstring_view ids(details->HardwareID, static_cast<size_t>(details->CompatIDsOffset) + details->CompatIDsLength);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), !ids.empty() && ids.back() != L'\0');
        if (!std::ranges::any_of(ids | std::views::split(L'\0'), [](const auto& id) {
                return wsl::shared::string::IsEqual(std::wstring_view(id.begin(), id.end()), c_winusbHardwareId, true);
            }))
        {
            continue;
        }

        wchar_t section[LINE_LEN]{};
        THROW_IF_WIN32_BOOL_FALSE(SetupDiGetActualSectionToInstallW(
            inf.get(), details->SectionName, section, ARRAYSIZE(section), nullptr, nullptr));
        if (SetupGetLineCountW(inf.get(), section) >= 0)
        {
            return S_OK;
        }
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_SECTION_NOT_FOUND);
    }
}
CATCH_RETURN()

} // namespace wsl::windows::common::usb