/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    registry.cpp

Abstract:

    This file contains registry management helper function implementation.

--*/

#include "precomp.h"
#include "registry.hpp"
#include "svccomm.hpp"
#pragma hdrstop

namespace {
std::wstring GetKeyPath(_In_ HKEY Key)
{
    if (Key == HKEY_LOCAL_MACHINE)
    {
        return L"HKLM";
    }
    else if (Key == HKEY_CLASSES_ROOT)
    {
        return L"HKCR";
    }
    else if (Key == HKEY_USERS)
    {
        return L"HKU";
    }
    else if (Key == HKEY_CURRENT_USER)
    {
        return L"HKCU";
    }
    else if (Key == HKEY_CURRENT_CONFIG)
    {
        return L"HKCC";
    }

    ULONG requiredSize{};
    auto status = ZwQueryKey(Key, KeyNameInformation, nullptr, 0, &requiredSize);
    if (status != STATUS_BUFFER_TOO_SMALL)
    {
        THROW_NTSTATUS(status);
    }

    std::vector<char> buffer(requiredSize, 0);

    status = ZwQueryKey(Key, KeyNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &requiredSize);
    THROW_IF_WIN32_ERROR(status);

    const auto* info = reinterpret_cast<KEY_NAME_INFORMATION*>(buffer.data());

    return std::wstring{info->Name, info->NameLength / sizeof(WCHAR)};
}

void ReportErrorIfFailed(_In_ LSTATUS Error, _In_ HKEY Key, _In_opt_ LPCWSTR Subkey, _In_opt_ LPCWSTR Value)
{
    if (Error == ERROR_SUCCESS)
    {
        return;
    }

    const auto result = HRESULT_FROM_WIN32(Error);
    if (Key == nullptr)
    {
        const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(result);
        THROW_HR_WITH_USER_ERROR(
            result, wsl::shared::Localization::MessageRegistryError(Subkey ? Subkey : L"[null]", errorString.c_str()).c_str());
    }

    auto path = GetKeyPath(Key);
    if (Subkey != nullptr)
    {
        path += L"\\" + std::wstring(Subkey);
    }

    if (Value != nullptr)
    {
        path += L"\\" + std::wstring(Value);
    }

    if (wsl::windows::common::ExecutionContext::ShouldCollectErrorMessage())
    {
        const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(result);
        THROW_HR_WITH_USER_ERROR(result, wsl::shared::Localization::MessageRegistryError(path.c_str(), errorString.c_str()).c_str());
    }
    else
    {
        THROW_HR_MSG(result, "An error occurred accessing the registry. Path: %ls", path.c_str());
    }
}
} // namespace
void wsl::windows::common::registry::ClearSubkeys(_In_ HKEY Key)
{
    for (const auto& e : EnumKeys(Key, KEY_READ))
    {
        DeleteKey(Key, e.first.c_str());
    }
}

wil::unique_hkey wsl::windows::common::registry::CreateKey(
    _In_ HKEY Key, _In_ LPCWSTR KeyName, _In_ REGSAM AccessMask, _Out_opt_ LPDWORD Disposition, _In_ DWORD Options)
{
    wil::unique_hkey NewKey;
    THROW_IF_WIN32_ERROR(RegCreateKeyExW(Key, KeyName, 0, nullptr, Options, AccessMask, nullptr, &NewKey, Disposition));

    return NewKey;
}

bool wsl::windows::common::registry::DeleteKey(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteTreeW(Key, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }

    return Result == NO_ERROR;
}

void wsl::windows::common::registry::DeleteKeyValue(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteKeyValueW(Key, nullptr, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }
}

void wsl::windows::common::registry::DeleteValue(_In_ HKEY Key, _In_ LPCWSTR KeyName)
{
    const LSTATUS Result = RegDeleteValueW(Key, KeyName);
    if (Result != ERROR_FILE_NOT_FOUND)
    {
        LOG_IF_WIN32_ERROR(Result);
    }
}

std::map<std::wstring, wil::unique_hkey> wsl::windows::common::registry::EnumKeys(_In_ HKEY Key, _In_ DWORD SubkeyAccess)
{
    // Get the max size of a subkey
    DWORD MaxSubkeySize = 0;
    QueryInfo(Key, &MaxSubkeySize);

    std::map<std::wstring, wil::unique_hkey> keys;
    for (DWORD Index = 0;; Index++)
    {
        std::wstring Name(MaxSubkeySize, '\0');
        DWORD NameSize = MaxSubkeySize + 1;
        const LSTATUS result = RegEnumKeyExW(Key, Index, Name.data(), &NameSize, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS)
        {
            break;
        }

        ReportErrorIfFailed(result, Key, nullptr, nullptr);

        Name.resize(NameSize);

        auto subKey = OpenKey(Key, Name.c_str(), SubkeyAccess);
        keys.emplace(std::move(Name), std::move(subKey));
    }

    return keys;
}

std::vector<std::pair<GUID, std::wstring>> wsl::windows::common::registry::EnumGuidKeys(_In_ HKEY Key)
{
    // Iterate through the provided keys and return a list of all sub-keys that are GUIDs.
    WCHAR buffer[39];
    std::vector<std::pair<GUID, std::wstring>> subKeys;
    DWORD index = 0;
    for (;;)
    {
        DWORD bufferSize = ARRAYSIZE(buffer);
        const LSTATUS error = RegEnumKeyExW(Key, index, buffer, &bufferSize, nullptr, nullptr, nullptr, nullptr);
        index += 1;
        if (error == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if ((error == ERROR_MORE_DATA) || ((error == ERROR_SUCCESS) && (bufferSize != (ARRAYSIZE(buffer) - 1))))
        {
            continue;
        }

        ReportErrorIfFailed(error, Key, nullptr, nullptr);

        // Ignore any subkeys that are not GUIDs.
        auto guid = wsl::shared::string::ToGuid(buffer);
        if (!guid.has_value())
        {
            continue;
        }

        subKeys.emplace_back(std::make_pair(guid.value(), std::wstring(buffer)));
    }

    return subKeys;
}

std::vector<std::pair<std::wstring, DWORD>> wsl::windows::common::registry::EnumValues(_In_ HKEY Key)
{
    std::vector<std::pair<std::wstring, DWORD>> values;
    DWORD maxValueNameSize = 0;
    QueryInfo(Key, nullptr, &maxValueNameSize);

    for (DWORD Index = 0;; Index++)
    {
        std::wstring valueName(maxValueNameSize, '\0');
        DWORD size = maxValueNameSize + 1;
        DWORD type = 0;

        const auto error = RegEnumValueW(Key, Index, valueName.data(), &size, nullptr, &type, nullptr, nullptr);
        if (error == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        THROW_IF_WIN32_ERROR(error);

        valueName.resize(size);
        values.emplace_back(std::move(valueName), type);
    }

    return values;
}

bool wsl::windows::common::registry::IsKeyVolatile(_In_ HKEY Key)
{
    KEY_FLAGS_INFORMATION info{};
    DWORD resultSize{};
    THROW_IF_NTSTATUS_FAILED(ZwQueryKey(Key, KeyFlagsInformation, &info, sizeof(info), &resultSize));

    return WI_IsFlagSet(info.KeyFlags, REG_OPTION_VOLATILE);
}

wil::unique_hkey wsl::windows::common::registry::OpenCurrentUser(_In_ REGSAM AccessMask)
{
    wil::unique_hkey UserKey;
    THROW_IF_WIN32_ERROR(RegOpenCurrentUser(AccessMask, &UserKey));

    return UserKey;
}

std::pair<wil::unique_hkey, HRESULT> wsl::windows::common::registry::OpenKeyNoThrow(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options)
{
    wil::unique_hkey OpenedKey;
    const auto error = RegOpenKeyExW(Key, SubKey, Options, AccessMask, &OpenedKey);

    return {std::move(OpenedKey), HRESULT_FROM_WIN32(error)};
}

wil::unique_hkey wsl::windows::common::registry::OpenKey(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options)
{
    auto [key, error] = OpenKeyNoThrow(Key, SubKey, AccessMask, Options);
    ReportErrorIfFailed(error, Key, SubKey, nullptr);

    return std::move(key);
}

wil::unique_hkey wsl::windows::common::registry::OpenLxssMachineKey(REGSAM AccessMask)
{
    wil::unique_hkey LxssKey = CreateKey(HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, AccessMask);
    THROW_LAST_ERROR_IF(!LxssKey);

    return LxssKey;
}

wil::unique_hkey wsl::windows::common::registry::OpenLxssUserKey()
{
    const wil::unique_hkey UserKey = OpenCurrentUser();
    wil::unique_hkey LxssKey = CreateKey(UserKey.get(), LXSS_REGISTRY_PATH);
    THROW_LAST_ERROR_IF(!LxssKey);

    return LxssKey;
}

wil::unique_hkey wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(_In_ PSID UserSid)
{
    // In this method we use the user SID to open a user specific key under HKLM
    // The reason for not using HKCU is that lxss trusts this key and will mount
    // all the volumes listed under it.
    // Given that only elevated users are allowed to mount disks, using HKCU would
    // create a security issue as non-admin users could write anything they want there.
    std::wstring path = std::format(L"{}\\{}", LXSS_DISK_MOUNTS_REGISTRY_PATH, wsl::windows::common::wslutil::SidToString(UserSid).get());

    // Create a volatile key so that disk states aren't kept after a reboot
    return CreateKey(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);
}

void wsl::windows::common::registry::QueryInfo(_In_ HKEY Key, _In_opt_ DWORD* MaxSubKeySize, _In_opt_ DWORD* MaxValueNameSize, _In_opt_ DWORD* MaxValueDataSize)
{
    const auto error = (RegQueryInfoKeyW(
        Key, nullptr, nullptr, nullptr, nullptr, MaxSubKeySize, nullptr, nullptr, MaxValueNameSize, MaxValueDataSize, nullptr, nullptr));

    ReportErrorIfFailed(error, Key, nullptr, nullptr);
}

DWORD
wsl::windows::common::registry::ReadDword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ DWORD DefaultValue)
{
    DWORD Returned = 0;
    DWORD Size = sizeof(Returned);
    const LONG Result = RegGetValueW(Key, KeyName, ValueName, RRF_RT_REG_DWORD, nullptr, &Returned, &Size);
    if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND))
    {
        return DefaultValue;
    }

    ReportErrorIfFailed(Result, Key, KeyName, ValueName);
    return Returned;
}

ULONG64
wsl::windows::common::registry::ReadQword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ ULONG64 DefaultValue)
{
    ULONG64 Returned = 0;
    DWORD Size = sizeof(Returned);
    const LONG Result = RegGetValueW(Key, KeyName, ValueName, RRF_RT_REG_QWORD, nullptr, &Returned, &Size);
    if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND))
    {
        return DefaultValue;
    }

    ReportErrorIfFailed(Result, Key, KeyName, ValueName);

    return Returned;
}

std::wstring wsl::windows::common::registry::ReadString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_opt_ LPCWSTR Default)
{
    auto value = ReadOptionalString(Key, KeyName, ValueName);
    if (!value.has_value())
    {
        if (ARGUMENT_PRESENT(Default))
        {
            return Default;
        }
        else
        {
            ReportErrorIfFailed(ERROR_PATH_NOT_FOUND, Key, KeyName, ValueName);
        }
    }

    return value.value();
}

std::optional<std::wstring> wsl::windows::common::registry::ReadOptionalString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName)
{
    DWORD Size = 0;
    LONG Result = RegGetValueW(Key, KeyName, ValueName, (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ), nullptr, nullptr, &Size);
    if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND) || (Size == 0))
    {
        return {};
    }

    ReportErrorIfFailed(Result, Key, KeyName, ValueName);

    //
    // Allocate a buffer and read the value of the key.
    //

    std::wstring Buffer(Size / sizeof(WCHAR), L'\0');
    Result = RegGetValueW(Key, KeyName, ValueName, (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ), nullptr, Buffer.data(), &Size);
    if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND) || (Size == 0))
    {
        return {};
    }

    ReportErrorIfFailed(Result, Key, KeyName, ValueName);

    Buffer.resize(wcsnlen(Buffer.c_str(), Buffer.size()));
    return Buffer;
}

std::vector<std::string> wsl::windows::common::registry::ReadStringSet(
    _In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, const std::vector<std::string>& Default)
{
    //
    // Detect if the key exists and determine how large of a buffer is needed.
    // If the key does not exist, return the default value.
    //

    LONG Result;
    DWORD Size = 0;
    Result = RegGetValueW(Key, KeyName, ValueName, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &Size);
    if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND) || (Size == 0))
    {
        //
        // Convert the supplied string into a vector of strings.
        //

        return Default;
    }

    ReportErrorIfFailed(Result, Key, KeyName, ValueName);

    //
    // Allocate a buffer to hold the value and two NULL terminators.
    //

    std::vector<WCHAR> Buffer(Size + 2);

    //
    // Read the value.
    //

    Result = RegGetValueW(Key, KeyName, ValueName, RRF_RT_REG_MULTI_SZ, nullptr, Buffer.data(), &Size);
    ReportErrorIfFailed(Result, Key, KeyName, ValueName);

    //
    // Convert the reg value into a vector of strings.
    //

    std::vector<std::string> Values{};
    for (auto Current = Buffer.data(); UNICODE_NULL != *Current; Current += wcslen(Current) + 1)
    {
        Values.push_back(wsl::shared::string::WideToMultiByte(Current));
    }

    return Values;
}

void wsl::windows::common::registry::WriteDword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ DWORD Value)
{
    const auto Result = RegSetKeyValueW(Key, SubKey, ValueName, REG_DWORD, &Value, sizeof(Value));
    ReportErrorIfFailed(Result, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteQword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ ULONG64 Value)
{
    const auto Result = RegSetKeyValueW(Key, SubKey, ValueName, REG_QWORD, &Value, sizeof(Value));
    ReportErrorIfFailed(Result, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteDefaultString(_In_ HKEY Key, _In_ LPCWSTR Value)
{
    SIZE_T StringLength = wcslen(Value);
    THROW_IF_FAILED(SizeTAdd(StringLength, 1, &StringLength));
    THROW_IF_FAILED(SizeTMult(StringLength, sizeof(WCHAR), &StringLength));

    THROW_HR_IF(E_INVALIDARG, (StringLength > (SIZE_T)DWORD_MAX));

    const auto Result = RegSetValueExW(Key, NULL, 0, REG_SZ, reinterpret_cast<const BYTE*>(Value), static_cast<DWORD>(StringLength));

    ReportErrorIfFailed(Result, Key, nullptr, nullptr);
}

void wsl::windows::common::registry::WriteString(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ LPCWSTR Value)
{
    SIZE_T StringLength = wcslen(Value);
    THROW_IF_FAILED(SizeTAdd(StringLength, 1, &StringLength));
    THROW_IF_FAILED(SizeTMult(StringLength, sizeof(WCHAR), &StringLength));

    THROW_HR_IF(E_INVALIDARG, (StringLength > (SIZE_T)DWORD_MAX));

    const auto Result = RegSetKeyValueW(Key, SubKey, ValueName, REG_SZ, Value, static_cast<DWORD>(StringLength));
    ReportErrorIfFailed(Result, Key, SubKey, ValueName);
}

void wsl::windows::common::registry::WriteStringSet(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR ValueName, _In_ const std::vector<std::wstring>& StringSet)
{
    THROW_HR_IF(E_INVALIDARG, (StringSet.size() == 0));

    //
    // Combine each element into a NULL-separated string ending with two NULL
    // terminators.
    //

    std::wstring Value;
    for (SIZE_T Index = 0; Index < StringSet.size(); Index += 1)
    {
        Value += StringSet[Index] + UNICODE_NULL;
    }

    Value += UNICODE_NULL;

    //
    // Ensure the wstring ends with two NULL terminators.
    //

    WI_ASSERT((Value.size() >= 2) && (Value.at(Value.size() - 1) == UNICODE_NULL) && (Value.at(Value.size() - 2) == UNICODE_NULL));

    //
    // Store the value in the registry.
    //

    SIZE_T ValueSize;
    THROW_IF_FAILED(SizeTMult(Value.size(), sizeof(WCHAR), &ValueSize));
    THROW_HR_IF(E_INVALIDARG, (ValueSize > (SIZE_T)DWORD_MAX));

    const auto Result = RegSetKeyValueW(Key, SubKey, ValueName, REG_MULTI_SZ, Value.c_str(), static_cast<DWORD>(ValueSize));
    ReportErrorIfFailed(Result, Key, SubKey, ValueName);
}