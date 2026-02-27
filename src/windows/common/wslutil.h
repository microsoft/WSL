/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslutil.h

Abstract:

    This file contains helper function declarations.

--*/

#pragma once
#include <functional>
#include <span>
#include <type_traits>
#include "SubProcess.h"
#include <winrt/windows.management.deployment.h>
#include "JsonUtils.h"

namespace wsl::windows::common {
struct Error;

struct ErrorStrings
{
    std::wstring Message;
    std::wstring Code;
};
} // namespace wsl::windows::common

namespace wsl::windows::common::wslutil {

// Namespace GUID used for Windows Terminal profile generation.
// {BE9372FE-59E1-4876-BDA9-C33C8F2F1AF1}
inline constexpr GUID WslTerminalNamespace = {0xbe9372fe, 0x59e1, 0x4876, {0xbd, 0xa9, 0xc3, 0x3c, 0x8f, 0x2f, 0x1a, 0xf1}};

// Namespace GUID for automatically generated Windows Terminal profiles.
// {2bde4a90-d05f-401c-9492-e40884ead1d8}
inline constexpr GUID GeneratedProfilesTerminalNamespace = {0x2bde4a90, 0xd05f, 0x401c, {0x94, 0x92, 0xe4, 0x8, 0x84, 0xea, 0xd1, 0xd8}};

inline auto c_msixPackageFamilyName = L"MicrosoftCorporationII.WindowsSubsystemForLinux_8wekyb3d8bbwe";
inline auto c_githubUrlOverrideRegistryValue = L"GitHubUrlOverride";
inline auto c_vhdFileExtension = L".vhd";
inline auto c_vhdxFileExtension = L".vhdx";
inline constexpr auto c_vmOwner = L"WSL";

struct GitHubReleaseAsset
{
    std::wstring url;
    uint64_t id{};
    std::wstring name;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GitHubReleaseAsset, url, id, name);
};

struct GitHubRelease
{
    std::wstring name;
    std::vector<GitHubReleaseAsset> assets;
    std::wstring created_at;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GitHubRelease, name, assets, created_at);
};

template <typename T>
void AssertValidPrintfArg()
{
    static_assert(std::is_fundamental_v<T> || std::is_same_v<wchar_t*, T> || std::is_same_v<char*, T> || std::is_same_v<HRESULT, T>);
}

int CallMsiPackage();

template <typename TInterface>
wil::com_ptr<TInterface> CoGetCallContext();

void CoInitializeSecurity();

void ConfigureCrt();

/// <summary>
/// Creates a COM server with user impersonation.
/// </summary>
template <typename Interface>
wil::com_ptr_t<Interface> CreateComServerAsUser(_In_ REFCLSID RefClsId, _In_ HANDLE UserToken)
{
    auto revert = wil::impersonate_token(UserToken);
    return wil::CoCreateInstance<Interface>(RefClsId, (CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING | CLSCTX_ENABLE_AAA));
}

template <typename Class, typename Interface>
wil::com_ptr_t<Interface> CreateComServerAsUser(_In_ HANDLE UserToken)
{
    return CreateComServerAsUser<Interface>(__uuidof(Class), UserToken);
}

std::wstring ConstructPipePath(_In_ std::wstring_view PipeName);

GUID CreateV5Uuid(const GUID& namespaceGuid, const std::span<const std::byte> name);

std::wstring DownloadFile(std::wstring_view Url, std::wstring Filename);

[[nodiscard]] HANDLE DuplicateHandle(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess = std::nullopt, _In_ BOOL InheritHandle = FALSE);

[[nodiscard]] HANDLE DuplicateHandleFromCallingProcess(_In_ HANDLE Handle);

[[nodiscard]] HANDLE DuplicateHandleToCallingProcess(_In_ HANDLE Handle, _In_ std::optional<DWORD> Permissions = {});

void EnforceFileLimit(LPCWSTR Folder, size_t limit, const std::function<bool(const std::filesystem::directory_entry&)>& pred);

std::wstring ErrorCodeToString(HRESULT Error);

ErrorStrings ErrorToString(const Error& error);

std::filesystem::path GetBasePath();

DWORD GetDefaultVersion(void);

std::wstring GetErrorString(_In_ HRESULT result);

std::optional<std::pair<std::wstring, GitHubReleaseAsset>> GetGitHubAssetFromRelease(const GitHubRelease& Release);

std::pair<std::wstring, GitHubReleaseAsset> GetLatestGitHubRelease(_In_ bool preRelease);

std::pair<std::wstring, GitHubReleaseAsset> GetLatestGitHubRelease(_In_ bool preRelease, _In_ LPCWSTR releases);

GitHubRelease GetGitHubReleaseByTag(_In_ const std::wstring& Version);

int GetLogicalProcessorCount();

std::optional<std::wstring> GetMsiPackagePath();

std::wstring GetPackageFamilyName(_In_ HANDLE process = GetCurrentProcess());

std::wstring GetSystemErrorString(_In_ HRESULT result);

std::wstring GetDebugShellPipeName(_In_ PSID Sid);

std::vector<BYTE> HashFile(HANDLE File, DWORD Algorithm);

void InitializeWil();

bool IsConsoleHandle(HANDLE Handle);

bool IsInteractiveConsole();

bool IsRunningInMsix();

bool IsVhdFile(_In_ const std::filesystem::path& path);

bool IsVirtualMachinePlatformInstalled();

std::vector<DWORD> ListRunningProcesses();

void MsiMessageCallback(INSTALLMESSAGE type, LPCWSTR message);

std::pair<wil::unique_hfile, wil::unique_hfile> OpenAnonymousPipe(DWORD Size, bool ReadPipeOverlapped, bool WritePipeOverlapped);

wil::unique_handle OpenCallingProcess(_In_ DWORD access);

std::tuple<uint32_t, uint32_t, uint32_t> ParseWslPackageVersion(_In_ const std::wstring& Version);

void PrintSystemError(_In_ HRESULT result, _Inout_ FILE* stream = stdout);

void PrintMessageImpl(_In_ const std::wstring& message, _In_ va_list& args, _Inout_ FILE* stream = stdout);

void PrintMessageImpl(_In_ const std::wstring& message, _Inout_ FILE* stream = stdout, ...);

void PrintMessage(_In_ const std::wstring& message, _Inout_ FILE* stream = stdout);

// This template is used to switch between the varargs and non-vararg versions of PrintMessage().
// If no varargs are passed, then the string shouldn't be used as a printf format specifier.
template <typename... Args>
void PrintMessage(_In_ const std::wstring& message, _Inout_ FILE* const stream = stdout, Args... args)
{
    static_assert(sizeof...(Args) > 0);

    // Validate that all Args are valid printf arguments.
    (
        [](auto e) {
            using T = decltype(e);
            if constexpr (std::is_pointer_v<T>)
            {
                AssertValidPrintfArg<std::add_pointer_t<std::remove_const_t<std::remove_pointer_t<T>>>>();
            }
            else
            {
                AssertValidPrintfArg<std::remove_const_t<T>>();
            }
        }(args),
        ...);

    PrintMessageImpl(message, stream, std::forward<Args>(args)...);
}

void SetCrtEncoding(int Mode);

void SetThreadDescription(LPCWSTR Name);

wil::unique_hfile ValidateFileSignature(LPCWSTR Path);

wil::unique_hlocal_string SidToString(_In_ PSID Sid);

int UpdatePackage(bool PreRelease, bool Repair);

UINT UpgradeViaMsi(_In_ LPCWSTR PackageLocation, _In_opt_ LPCWSTR ExtraArgs, _In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

UINT UninstallViaMsi(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

void WriteInstallLog(const std::string& Content);

winrt::Windows::Management::Deployment::PackageVolume GetSystemVolume();

} // namespace wsl::windows::common::wslutil
