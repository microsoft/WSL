/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    registry.hpp

Abstract:

    This is the header file for registry management helper functions.

--*/

#pragma once

#define LXSS_SERVICE_REGISTRY_PATH L"SYSTEM\\CurrentControlSet\\Services\\LxssManager"

namespace wsl::windows::common::registry {

void ClearSubkeys(_In_ HKEY Key);

wil::unique_hkey CreateKey(
    _In_ HKEY Key, _In_ LPCWSTR KeyName, _In_ REGSAM AccessMask = (KEY_READ | KEY_WRITE), _Out_opt_ LPDWORD Disposition = nullptr, _In_ DWORD Options = 0);

bool DeleteKey(_In_ HKEY Key, _In_ LPCWSTR KeyName);

void DeleteKeyValue(_In_ HKEY Key, _In_ LPCWSTR KeyName);

void DeleteValue(_In_ HKEY Key, _In_ LPCWSTR KeyName);

std::vector<std::pair<GUID, std::wstring>> EnumGuidKeys(_In_ HKEY LxssKey);

std::map<std::wstring, wil::unique_hkey> EnumKeys(_In_ HKEY Key, _In_ DWORD SubkeyAccess);

std::vector<std::pair<std::wstring, DWORD>> EnumValues(_In_ HKEY Key);

DWORD GetMachinePolicyValue(_In_ LPCWSTR Name, HKEY lxssKey);

bool IsKeyVolatile(_In_ HKEY Key);

wil::unique_hkey OpenCurrentUser(_In_ REGSAM AccessMask = (KEY_READ | KEY_WRITE));

wil::unique_hkey OpenKey(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options = 0);

std::pair<wil::unique_hkey, HRESULT> OpenKeyNoThrow(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ REGSAM AccessMask, _In_ DWORD Options = 0);

wil::unique_hkey OpenLxssMachineKey(REGSAM AccessMask = KEY_READ);

wil::unique_hkey OpenLxssUserKey();

wil::unique_hkey OpenOrCreateLxssDiskMountsKey(_In_ PSID UserSid);

void QueryInfo(_In_ HKEY Key, _In_opt_ DWORD* MaxSubKeySize = nullptr, _In_opt_ DWORD* MaxValueNameSize = nullptr, _In_opt_ DWORD* MaxValueDataSize = nullptr);

DWORD
ReadDword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ DWORD DefaultValue);

ULONG64
ReadQword(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ ULONG64 DefaultValue);

std::wstring ReadString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_opt_ LPCWSTR Default = nullptr);
std::optional<std::wstring> ReadOptionalString(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName);

std::vector<std::string> ReadStringSet(_In_ HKEY Key, _In_opt_ LPCWSTR KeyName, _In_opt_ LPCWSTR ValueName, _In_ const std::vector<std::string>& Default);

void WriteDword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR KeyName, _In_ DWORD Value);

void WriteQword(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR KeyName, _In_ ULONG64 Value);

void WriteDefaultString(_In_ HKEY Key, _In_ LPCWSTR Value);

void WriteString(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR KeyName, _In_ LPCWSTR Value);

void WriteStringSet(_In_ HKEY Key, _In_ LPCWSTR SubKey, _In_ LPCWSTR KeyName, _In_ const std::vector<std::wstring>& StringSet);

} // namespace wsl::windows::common::registry
