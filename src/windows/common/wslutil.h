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
#include "wslc.h"

namespace wsl::windows::common {
struct Error;

struct ErrorStrings
{
    std::wstring Message;
    std::wstring Code;
    std::optional<std::wstring> Source;
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
inline constexpr auto c_vmOwner = L"WSL"; // TODO-WSLC: Does this apply to WSLC ?

enum class EnumReferenceFormat
{
    None,
    Tag,
    Digest
};

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

struct COMErrorInfo
{
    wil::unique_bstr Message;
    wil::unique_bstr Source;
};

static_assert(sizeof(WSLCHandle::Handle) == sizeof(HANDLE));
static_assert(sizeof(FILE_HANDLE) == sizeof(HANDLE));
static_assert(sizeof(PIPE_HANDLE) == sizeof(HANDLE));
static_assert(sizeof(SOCKET_HANDLE) == sizeof(HANDLE));

struct COMOutputHandle : public WSLCHandle
{
    NON_COPYABLE(COMOutputHandle);
    NON_MOVABLE(COMOutputHandle);
    COMOutputHandle()
    {
        ZeroMemory(&Handle, sizeof(Handle));
        Type = WSLCHandleTypeUnknown;
    }

    ~COMOutputHandle()
    {
        Reset();
    }

    void Reset() noexcept
    {
        if (!Empty())
        {
            LOG_IF_WIN32_BOOL_FALSE(CloseHandle(Handle.File));
            Handle.File = nullptr;
        }
    }

    [[nodiscard]] wil::unique_handle Release() noexcept
    {
        wil::unique_handle handle(Handle.File);
        Handle.File = nullptr;

        return handle;
    }

    HANDLE Get() const noexcept
    {
        return Handle.File;
    }

    bool Empty() const noexcept
    {
        return Handle.File == nullptr || Handle.File == INVALID_HANDLE_VALUE;
    }
};

struct PruneResult
{
    NON_COPYABLE(PruneResult);
    WSLCPruneContainersResults result{};

    PruneResult() = default;

    PruneResult(PruneResult&& other)
    {
        *this = std::move(other);
    }

    PruneResult& operator=(PruneResult&& other)
    {
        CoTaskMemFree(result.Containers);
        result.Containers = other.result.Containers;
        result.ContainersCount = other.result.ContainersCount;
        result.SpaceReclaimed = other.result.SpaceReclaimed;

        other.result.Containers = nullptr;
        other.result.ContainersCount = 0;
        other.result.SpaceReclaimed = 0;

        return *this;
    }

    ~PruneResult()
    {
        CoTaskMemFree(result.Containers);
    }
};

class StopWatch
{
    NON_COPYABLE(StopWatch);
    NON_MOVABLE(StopWatch);

public:
    StopWatch() = default;

    uint64_t ElapsedMilliseconds() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_startTime).count();
    }

private:
    std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();
};

template <typename T>
void AssertValidPrintfArg()
{
    static_assert(std::is_fundamental_v<T> || std::is_same_v<wchar_t*, T> || std::is_same_v<char*, T> || std::is_same_v<HRESULT, T>);
}

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

std::wstring DownloadFileImpl(std::wstring_view Url, std::wstring Filename, const std::function<void(uint64_t, uint64_t)>& Progress);

[[nodiscard]] HANDLE DuplicateHandle(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess = std::nullopt, _In_ BOOL InheritHandle = FALSE);

[[nodiscard]] HANDLE DuplicateHandleFromCallingProcess(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess = {});

[[nodiscard]] HANDLE DuplicateHandleToCallingProcess(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess = {});

void EnforceFileLimit(LPCWSTR Folder, size_t limit, const std::function<bool(const std::filesystem::directory_entry&)>& pred);

std::wstring ErrorCodeToString(HRESULT Error);

ErrorStrings ErrorToString(const Error& error);

[[nodiscard]] HANDLE FromCOMInputHandle(WSLCHandle Handle);

std::filesystem::path GetBasePath();

std::optional<COMErrorInfo> GetCOMErrorInfo();

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

std::optional<std::tuple<uint32_t, uint32_t, uint32_t>> GetInstalledPackageVersion();

std::vector<BYTE> HashFile(HANDLE File, DWORD Algorithm);

void InitializeWil();

bool IsConsoleHandle(HANDLE Handle);

bool IsInteractiveConsole();

bool IsRunningInMsix();

bool IsVhdFile(_In_ const std::filesystem::path& path);

bool IsVirtualMachinePlatformInstalled();

std::vector<DWORD> ListRunningProcesses();

std::pair<std::string, std::string> NormalizeRepo(const std::string& Input);

std::pair<wil::unique_hfile, wil::unique_hfile> OpenAnonymousPipe(DWORD Size, bool ReadPipeOverlapped, bool WritePipeOverlapped);

wil::unique_handle OpenCallingProcess(_In_ DWORD access);

void ParseIpv4Address(const char* Address, in_addr& Result);

void ParseIpv6Address(const char* Address, in_addr6& Result);

std::tuple<uint32_t, uint32_t, uint32_t> ParseWslPackageVersion(_In_ const std::wstring& Version);

std::pair<std::string, std::optional<std::string>> ParseImage(const std::string& Input, EnumReferenceFormat* Format = nullptr);

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

wil::unique_hlocal_string SidToString(_In_ PSID Sid);

WSLCHandle ToCOMInputHandle(HANDLE Handle);
[[nodiscard]] WSLCHandle ToCOMOutputHandle(HANDLE Handle, DWORD Access);
[[nodiscard]] WSLCHandle ToCOMOutputHandle(HANDLE Handle, DWORD Access, WSLCHandleType Type);

winrt::Windows::Management::Deployment::PackageVolume GetSystemVolume();

std::string Base64Encode(const std::string& input);
std::string Base64Decode(const std::string& encoded);

// Builds the base64-encoded X-Registry-Auth header value used by Docker APIs
// (PullImage, PushImage, etc.) from the given credentials.
std::string BuildRegistryAuthHeader(const std::string& username, const std::string& password);

// Builds the base64-encoded X-Registry-Auth header value from an identity token
// returned by Authenticate().
std::string BuildRegistryAuthHeader(const std::string& identityToken);

std::map<std::string, std::string> ParseKeyValuePairs(_In_reads_opt_(count) const KeyValuePair* pairs, ULONG count, _In_opt_ LPCSTR reservedKey = nullptr);

} // namespace wsl::windows::common::wslutil
