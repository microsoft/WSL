/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslcsdk.cpp

Abstract:

    This file contains the public WSLC Client SDK api implementations.

--*/
#include "precomp.h"

#include "wslcsdk.h"
#include "WslcsdkPrivate.h"
#include "ProgressCallback.h"
#include "TerminationCallback.h"
#include "wslutil.h"

using namespace std::string_view_literals;
using namespace wsl::windows::common::wslutil;

namespace {
constexpr uint32_t s_DefaultCPUCount = 2;
constexpr uint32_t s_DefaultMemoryMB = 2000;
// Maximum value per use with HVSOCKET_CONNECT_TIMEOUT_MAX
constexpr ULONG s_DefaultBootTimeout = 300000;
// Default to 1 GB
constexpr UINT64 s_DefaultStorageSize = 1000 * 1000 * 1000;

#define WSLC_FLAG_VALUE_ASSERT(_wlsc_name_, _wslc_name_) \
    static_assert(_wlsc_name_ == _wslc_name_, "Flag values differ: " #_wlsc_name_ " != " #_wslc_name_);

template <typename Flags>
struct FlagsTraits
{
    static_assert(false, "Flags used without traits defined.");
};

template <>
struct FlagsTraits<WslcSessionFeatureFlags>
{
    using WslcType = WSLCFeatureFlags;
    constexpr static WslcSessionFeatureFlags Mask = WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU;
    WSLC_FLAG_VALUE_ASSERT(WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU, WslcFeatureFlagsGPU);
};

template <>
struct FlagsTraits<WslcContainerFlags>
{
    using WslcType = WSLCContainerFlags;
    constexpr static WslcContainerFlags Mask = WSLC_CONTAINER_FLAG_AUTO_REMOVE | WSLC_CONTAINER_FLAG_ENABLE_GPU;
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_FLAG_AUTO_REMOVE, WSLCContainerFlagsRm);
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_FLAG_ENABLE_GPU, WSLCContainerFlagsGpu);
    // TODO: WSLC_CONTAINER_FLAG_PRIVILEGED has no associated runtime value
};

template <>
struct FlagsTraits<WslcContainerStartFlags>
{
    using WslcType = WSLCContainerStartFlags;
    constexpr static WslcContainerStartFlags Mask = WSLC_CONTAINER_START_FLAG_ATTACH;
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_START_FLAG_ATTACH, WSLCContainerStartFlagsAttach);
};

template <>
struct FlagsTraits<WslcDeleteContainerFlags>
{
    using WslcType = WSLCDeleteFlags;
    constexpr static WslcDeleteContainerFlags Mask = WSLC_DELETE_CONTAINER_FLAG_FORCE;
    WSLC_FLAG_VALUE_ASSERT(WSLC_DELETE_CONTAINER_FLAG_FORCE, WSLCDeleteFlagsForce);
};

template <typename Flags>
typename FlagsTraits<Flags>::WslcType ConvertFlags(Flags flags)
{
    using traits = FlagsTraits<Flags>;
    return static_cast<typename traits::WslcType>(flags & traits::Mask);
}

WSLCSignal Convert(WslcSignal signal)
{
    switch (signal)
    {
    case WSLC_SIGNAL_NONE:
        return WSLCSignal::WSLCSignalNone;
    case WSLC_SIGNAL_SIGHUP:
        return WSLCSignal::WSLCSignalSIGHUP;
    case WSLC_SIGNAL_SIGINT:
        return WSLCSignal::WSLCSignalSIGINT;
    case WSLC_SIGNAL_SIGQUIT:
        return WSLCSignal::WSLCSignalSIGQUIT;
    case WSLC_SIGNAL_SIGKILL:
        return WSLCSignal::WSLCSignalSIGKILL;
    case WSLC_SIGNAL_SIGTERM:
        return WSLCSignal::WSLCSignalSIGTERM;
    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid WslcSignal: %i", signal);
    }
}

WSLCContainerNetworkType Convert(WslcContainerNetworkingMode mode)
{
    switch (mode)
    {
    case WSLC_CONTAINER_NETWORKING_MODE_NONE:
        return WSLCContainerNetworkTypeNone;
    case WSLC_CONTAINER_NETWORKING_MODE_BRIDGED:
        return WSLCContainerNetworkTypeBridged;
    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid WslcContainerNetworkingMode: %i", mode);
    }
}

void ConvertSHA256Hash(const char* hashString, uint8_t sha256[32])
{
    static constexpr std::string_view s_sha256Prefix = "sha256:"sv;
    static constexpr size_t s_sha256ByteCount = 32;

    THROW_HR_IF_NULL(E_POINTER, sha256);

    if (!hashString)
    {
        return;
    }

    std::string_view hashStringView{hashString};
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        hashStringView.length() < s_sha256Prefix.length() || hashStringView.substr(0, s_sha256Prefix.length()) != s_sha256Prefix,
        "Unexpected hash specifier: %hs",
        hashString);

    auto hashBytes = wsl::windows::common::string::HexToBytes(hashStringView.substr(s_sha256Prefix.length()));
    THROW_HR_IF_MSG(E_INVALIDARG, hashBytes.size() != s_sha256ByteCount, "SHA256 hash was not 32 bytes: %zu", hashBytes.size());
    memcpy(sha256, &hashBytes[0], s_sha256ByteCount);
}

// TODO: Replace with a derivation of wsl::windows::common::ExecutionContext when telemetry changes are introduced
//       This will make usage even easier as we can just use the WIL result macros directly.
struct ErrorInfoWrapper
{
    ErrorInfoWrapper(PWSTR* errorMessage) : m_errorMessage(errorMessage)
    {
        if (m_errorMessage)
        {
            *m_errorMessage = nullptr;
        }
    }

    void GetErrorInfoFromCOM()
    {
        if (m_errorMessage)
        {
            auto errorInfo = wsl::windows::common::wslutil::GetCOMErrorInfo();
            if (errorInfo)
            {
                *m_errorMessage = wil::make_unique_string<wil::unique_cotaskmem_string>(errorInfo->Message.get()).release();
            }
        }
    }

    HRESULT CaptureResult(HRESULT hr)
    {
        m_hr = hr;
        if (FAILED_LOG(m_hr.value()))
        {
            GetErrorInfoFromCOM();
        }
        return m_hr.value();
    }

    operator HRESULT() const
    {
        THROW_HR_IF(E_UNEXPECTED, !m_hr);
        return m_hr.value();
    }

private:
    PWSTR* m_errorMessage = nullptr;
    std::optional<HRESULT> m_hr;
};

void EnsureAbsolutePath(const std::filesystem::path& path, bool containerPath)
{
    THROW_HR_IF(E_INVALIDARG, path.empty());

    if (containerPath)
    {
        auto pathString = path.native();
        // Not allowed to mount to root
        THROW_HR_IF(E_INVALIDARG, pathString.length() < 2);
        // Must be absolute
        THROW_HR_IF(E_INVALIDARG, pathString[0] != L'/');
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, path.is_relative());
    }
}

bool CopyProcessSettingsToRuntime(WSLCProcessOptions& runtimeOptions, const WslcContainerProcessOptionsInternal* initProcessOptions)
{
    if (initProcessOptions)
    {
        runtimeOptions.CurrentDirectory = initProcessOptions->currentDirectory;
        runtimeOptions.CommandLine.Values = initProcessOptions->commandLine;
        runtimeOptions.CommandLine.Count = initProcessOptions->commandLineCount;
        runtimeOptions.Environment.Values = initProcessOptions->environment;
        runtimeOptions.Environment.Count = initProcessOptions->environmentCount;

        // TODO: No user access
        // containerOptions.InitProcessOptions.Flags;
        // containerOptions.InitProcessOptions.TtyRows;
        // containerOptions.InitProcessOptions.TtyColumns;
        // containerOptions.InitProcessOptions.User;

        return true;
    }
    else
    {
        return false;
    }
}

// Normalizes file inputs to HANDLE+length.
struct ImageFileResolver
{
    ImageFileResolver(PCWSTR path) : m_fileHandle(INVALID_HANDLE_VALUE)
    {
        THROW_HR_IF_NULL(E_POINTER, path);

        wil::unique_handle imageFileHandle{
            CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!imageFileHandle);

        LARGE_INTEGER fileSize{};
        THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(imageFileHandle.get(), &fileSize));

        m_fileHandle = std::move(imageFileHandle);
        m_length = static_cast<ULONGLONG>(fileSize.QuadPart);
    }

    ImageFileResolver(HANDLE imageContent, uint64_t imageContentLength) : m_fileHandle(imageContent)
    {
        THROW_HR_IF(E_INVALIDARG, imageContent == nullptr || imageContent == INVALID_HANDLE_VALUE);
        THROW_HR_IF(E_INVALIDARG, imageContentLength == 0);

        m_length = imageContentLength;
    }

    HANDLE Handle() const
    {
        return m_fileHandle.Get();
    }

    ULONGLONG Length() const
    {
        return m_length;
    }

private:
    wsl::windows::common::relay::HandleWrapper m_fileHandle;
    ULONGLONG m_length;
};

// TODO: Implement Server SKU specific checks
bool NeedsVirtualMachineServicesInstalled()
{
    return !wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled();
}

#define WSLC_API_MIN_VERSION_SUPPORTED 2, 8, 0

bool DoesWslRuntimeVersionSupportWslc(const std::optional<std::tuple<uint32_t, uint32_t, uint32_t>>& version)
{
    constexpr auto minimalPackageVersion = std::tuple<uint32_t, uint32_t, uint32_t>{WSLC_API_MIN_VERSION_SUPPORTED};
    return version.has_value() && version >= minimalPackageVersion;
}

enum class WslRuntimeState
{
    NotInstalled,
    InstalledWithoutWslcSupport,
    InstalledWithWslcSupport,
};

WslRuntimeState CheckWslRuntimeState()
{
    auto version = wsl::windows::common::wslutil::GetInstalledPackageVersion();

    if (!version.has_value())
    {
        return WslRuntimeState::NotInstalled;
    }

    return DoesWslRuntimeVersionSupportWslc(version) ? WslRuntimeState::InstalledWithWslcSupport : WslRuntimeState::InstalledWithoutWslcSupport;
}

std::pair<wil::com_ptr<IWSLCSessionManager>, HRESULT> CreateSessionManagerRaw()
{
    wil::com_ptr<IWSLCSessionManager> result;
    HRESULT hr = CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&result));
    return {result, hr};
}

wil::com_ptr<IWSLCSessionManager> CreateSessionManager()
{
    auto [result, hr] = CreateSessionManagerRaw();

    if (hr == REGDB_E_CLASSNOTREG)
    {
        WslRuntimeState currentState = CheckWslRuntimeState();
        THROW_WIN32_IF_MSG(
            ERROR_NOT_SUPPORTED,
            currentState == WslRuntimeState::InstalledWithoutWslcSupport,
            "The currently installed WSL version does not support WSLC.");
        THROW_HR_IF_MSG(
            hr,
            currentState == WslRuntimeState::InstalledWithWslcSupport,
            "The WSL install appears to be corrupted; session manager class was not registered.");
    }

    THROW_IF_FAILED(hr);

    wsl::windows::common::security::ConfigureForCOMImpersonation(result.get());

    return result;
}

bool NeedsWslRuntimeInstalled()
{
    auto hr = CreateSessionManagerRaw().second;

    if (SUCCEEDED(hr))
    {
        return false;
    }
    else if (hr == REGDB_E_CLASSNOTREG)
    {
        return true;
    }
    THROW_HR(hr);
}
} // namespace

// SESSION DEFINITIONS
STDAPI WslcInitSessionSettings(_In_ PCWSTR name, _In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings)
try
{
    RETURN_HR_IF_NULL(E_POINTER, name);
    RETURN_HR_IF_NULL(E_POINTER, storagePath);

    auto internalType = CheckAndGetInternalType(sessionSettings);

    *internalType = {};

    internalType->displayName = name;
    internalType->storagePath = storagePath;
    internalType->cpuCount = s_DefaultCPUCount;
    internalType->memoryMb = s_DefaultMemoryMB;
    internalType->timeoutMS = s_DefaultBootTimeout;
    internalType->vhdRequirements.sizeInBytes = s_DefaultStorageSize;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (cpuCount)
    {
        internalType->cpuCount = cpuCount;
    }
    else
    {
        internalType->cpuCount = s_DefaultCPUCount;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMb)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (memoryMb)
    {
        internalType->memoryMb = memoryMb;
    }
    else
    {
        internalType->memoryMb = s_DefaultMemoryMB;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcCreateSession(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    RETURN_HR_IF_NULL(E_POINTER, session);
    *session = nullptr;
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(sessionSettings);

    wil::com_ptr<IWSLCSessionManager> sessionManager = CreateSessionManager();

    auto result = std::make_unique<WslcSessionImpl>();
    WSLCSessionSettings runtimeSettings{};
    runtimeSettings.DisplayName = internalType->displayName;
    runtimeSettings.StoragePath = internalType->storagePath;
    runtimeSettings.MaximumStorageSizeMb = internalType->vhdRequirements.sizeInBytes / _1MB;
    runtimeSettings.CpuCount = internalType->cpuCount;
    runtimeSettings.MemoryMb = internalType->memoryMb;
    runtimeSettings.BootTimeoutMs = internalType->timeoutMS;
    runtimeSettings.NetworkingMode = WSLCNetworkingModeVirtioProxy;
    auto terminationCallback = TerminationCallback::CreateIf(internalType);
    if (terminationCallback)
    {
        result->terminationCallback.attach(terminationCallback.as<ITerminationCallback>().detach());
        runtimeSettings.TerminationCallback = terminationCallback.get();
    }
    runtimeSettings.FeatureFlags = ConvertFlags(internalType->featureFlags);
    WI_SetFlag(runtimeSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);

    if (SUCCEEDED(errorInfoWrapper.CaptureResult(sessionManager->CreateSession(&runtimeSettings, WSLCSessionFlagsNone, &result->session))))
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->session.get());
        *session = reinterpret_cast<WslcSession>(result.release());
    }

    return errorInfoWrapper;
}
CATCH_RETURN();

STDAPI WslcTerminateSession(_In_ WslcSession session)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);

    RETURN_HR(internalType->session->Terminate());
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (timeoutMS)
    {
        internalType->timeoutMS = timeoutMS;
    }
    else
    {
        internalType->timeoutMS = s_DefaultBootTimeout;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcCreateSessionVhdVolume(_In_ WslcSession session, _In_ const WslcVhdRequirements* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};

    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);

    RETURN_HR_IF_NULL(E_INVALIDARG, options->name);
    RETURN_HR_IF(E_INVALIDARG, options->sizeInBytes == 0);
    RETURN_HR_IF(E_NOTIMPL, options->type != WSLC_VHD_TYPE_DYNAMIC);

    WSLCVolumeOptions volumeOptions{};
    volumeOptions.Name = options->name;
    // Only supported value currently
    volumeOptions.Type = "vhd";

    auto dynamicOptions = std::format(R"({{ "SizeBytes": "{}" }})", options->sizeInBytes);
    volumeOptions.Options = dynamicOptions.c_str();

    return errorInfoWrapper.CaptureResult(internalType->session->CreateVolume(&volumeOptions));
}
CATCH_RETURN();

STDAPI WslcDeleteSessionVhdVolume(_In_ WslcSession session, _In_z_ PCSTR name, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};

    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, name);

    return errorInfoWrapper.CaptureResult(internalType->session->DeleteVolume(name));
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsVhd(_In_ WslcSessionSettings* sessionSettings, _In_opt_ const WslcVhdRequirements* vhdRequirements)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (vhdRequirements)
    {
        RETURN_HR_IF(E_INVALIDARG, vhdRequirements->sizeInBytes == 0);
        RETURN_HR_IF(E_NOTIMPL, vhdRequirements->type != WSLC_VHD_TYPE_DYNAMIC);

        internalType->vhdRequirements = *vhdRequirements;
    }
    else
    {
        internalType->vhdRequirements = {};
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    internalType->featureFlags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetSessionSettingsTerminationCallback(
    _In_ WslcSessionSettings* sessionSettings, _In_opt_ WslcSessionTerminationCallback terminationCallback, _In_opt_ PVOID terminationContext)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);
    RETURN_HR_IF(E_INVALIDARG, terminationCallback == nullptr && terminationContext != nullptr);

    internalType->terminationCallback = terminationCallback;
    internalType->terminationCallbackContext = terminationContext;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcReleaseSession(_In_ WslcSession session)
try
{
    auto internalType = CheckAndGetInternalTypeUniquePointer(session);

    // Intentionally destroy session before termination callback in the event that
    // the termination callback ends up being invoked by session destruction.
    internalType->session.reset();
    internalType->terminationCallback.reset();

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcReleaseContainer(_In_ WslcContainer container)
try
{
    CheckAndGetInternalTypeUniquePointer(container);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcReleaseProcess(_In_ WslcProcess process)
try
{
    CheckAndGetInternalTypeUniquePointer(process);

    return S_OK;
}
CATCH_RETURN();

// CONTAINER DEFINITIONS

STDAPI WslcInitContainerSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF_NULL(E_POINTER, imageName);

    *internalType = {};

    internalType->image = imageName;
    // Default network configuration to WSLC SDK `0`, which is NONE.
    internalType->networking = WSLCContainerNetworkTypeNone;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcCreateContainer(_In_ WslcSession session, _In_ const WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    RETURN_HR_IF_NULL(E_POINTER, container);
    *container = nullptr;
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalSession = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalSession->session);
    auto internalContainerSettings = CheckAndGetInternalType(containerSettings);

    auto result = std::make_unique<WslcContainerImpl>();

    WSLCContainerOptions containerOptions{};
    containerOptions.Image = internalContainerSettings->image;
    containerOptions.Name = internalContainerSettings->runtimeName;
    containerOptions.HostName = internalContainerSettings->HostName;
    containerOptions.DomainName = internalContainerSettings->DomainName;
    containerOptions.Flags = ConvertFlags(internalContainerSettings->containerFlags);

    CopyProcessSettingsToRuntime(containerOptions.InitProcessOptions, internalContainerSettings->initProcessOptions);

    std::unique_ptr<WSLCVolume[]> convertedVolumes;
    if (internalContainerSettings->volumes && internalContainerSettings->volumesCount)
    {
        convertedVolumes = std::make_unique<WSLCVolume[]>(internalContainerSettings->volumesCount);
        for (uint32_t i = 0; i < internalContainerSettings->volumesCount; ++i)
        {
            const WslcContainerVolume& internalVolume = internalContainerSettings->volumes[i];
            WSLCVolume& convertedVolume = convertedVolumes[i];

            convertedVolume.HostPath = internalVolume.windowsPath;
            convertedVolume.ContainerPath = internalVolume.containerPath;
            convertedVolume.ReadOnly = internalVolume.readOnly;
        }
        containerOptions.Volumes = convertedVolumes.get();
        containerOptions.VolumesCount = static_cast<ULONG>(internalContainerSettings->volumesCount);
    }

    std::unique_ptr<WSLCNamedVolume[]> convertedNamedVolumes;
    if (internalContainerSettings->namedVolumes && internalContainerSettings->namedVolumesCount)
    {
        convertedNamedVolumes = std::make_unique<WSLCNamedVolume[]>(internalContainerSettings->namedVolumesCount);
        for (uint32_t i = 0; i < internalContainerSettings->namedVolumesCount; ++i)
        {
            const WslcContainerNamedVolume& internalVolume = internalContainerSettings->namedVolumes[i];
            WSLCNamedVolume& convertedVolume = convertedNamedVolumes[i];

            convertedVolume.Name = internalVolume.name;
            convertedVolume.ContainerPath = internalVolume.containerPath;
            convertedVolume.ReadOnly = internalVolume.readOnly;
        }
        containerOptions.NamedVolumes = convertedNamedVolumes.get();
        containerOptions.NamedVolumesCount = static_cast<ULONG>(internalContainerSettings->namedVolumesCount);
    }

    std::unique_ptr<WSLCPortMapping[]> convertedPorts;
    if (internalContainerSettings->ports && internalContainerSettings->portsCount)
    {
        convertedPorts = std::make_unique<WSLCPortMapping[]>(internalContainerSettings->portsCount);
        for (uint32_t i = 0; i < internalContainerSettings->portsCount; ++i)
        {
            const WslcContainerPortMapping& internalPort = internalContainerSettings->ports[i];
            WSLCPortMapping& convertedPort = convertedPorts[i];

            convertedPort.HostPort = internalPort.windowsPort;
            convertedPort.ContainerPort = internalPort.containerPort;

            // TODO: Ipv6 & custom binding address support.
            convertedPort.Family = AF_INET;

            // TODO: Consider using standard protocol numbers instead of our own enum.
            convertedPort.Protocol = internalPort.protocol == WSLC_PORT_PROTOCOL_TCP ? IPPROTO_TCP : IPPROTO_UDP;
            strcpy_s(convertedPort.BindingAddress, "127.0.0.1");
        }
        containerOptions.Ports = convertedPorts.get();
        containerOptions.PortsCount = static_cast<ULONG>(internalContainerSettings->portsCount);
    }

    containerOptions.ContainerNetwork.ContainerNetworkType = internalContainerSettings->networking;

    if (internalContainerSettings->entrypoint && internalContainerSettings->entrypointCount)
    {
        containerOptions.Entrypoint.Values = internalContainerSettings->entrypoint;
        containerOptions.Entrypoint.Count = internalContainerSettings->entrypointCount;
    }

    // TODO: No user access
    // containerOptions.Labels;
    // containerOptions.LabelsCount;
    // containerOptions.StopSignal;
    // containerOptions.ShmSize;

    if (SUCCEEDED(errorInfoWrapper.CaptureResult(internalSession->session->CreateContainer(&containerOptions, &result->container))))
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->container.get());

        if (IOCallback::HasIOCallback(internalContainerSettings->initProcessOptions))
        {
            result->ioCallbackOptions = internalContainerSettings->initProcessOptions->ioCallbacks;
        }

        *container = reinterpret_cast<WslcContainer>(result.release());
    }

    return errorInfoWrapper;
}
CATCH_RETURN();

STDAPI WslcStartContainer(_In_ WslcContainer container, _In_ WslcContainerStartFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    bool hasIOCallback = IOCallback::HasIOCallback(internalType->ioCallbackOptions);
    // If callbacks were provided, ATTACH must be used.
    // TODO: Consider if we should just override flags when callbacks were provided instead.
    RETURN_HR_IF(E_INVALIDARG, WI_IsFlagClear(flags, WSLC_CONTAINER_START_FLAG_ATTACH) && hasIOCallback);

    if (SUCCEEDED(errorInfoWrapper.CaptureResult(internalType->container->Start(ConvertFlags(flags), nullptr))))
    {
        if (hasIOCallback)
        {
            wil::com_ptr<IWSLCProcess> process;
            RETURN_IF_FAILED(internalType->container->GetInitProcess(&process));
            wsl::windows::common::security::ConfigureForCOMImpersonation(process.get());
            internalType->ioCallbacks = std::make_shared<IOCallback>(process.get(), internalType->ioCallbackOptions);
        }
    }

    return errorInfoWrapper;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->containerFlags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsEntrypoint(_In_ WslcContainerSettings* containerSettings, _In_reads_(argc) PCSTR const* argv, _In_ size_t argc)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF(
        E_INVALIDARG,
        (argv == nullptr && argc != 0) || (argv != nullptr && argc == 0) ||
            (argc > static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    internalType->entrypoint = argv;
    internalType->entrypointCount = static_cast<uint32_t>(argc);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->runtimeName = name;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->HostName = hostName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->DomainName = domainName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->initProcessOptions = GetInternalType(initProcess);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->networking = Convert(networkingMode);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsPortMappings(
    _In_ WslcContainerSettings* containerSettings, _In_reads_opt_(portMappingCount) const WslcContainerPortMapping* portMappings, _In_ uint32_t portMappingCount)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF(E_INVALIDARG, (portMappings == nullptr && portMappingCount != 0) || (portMappings != nullptr && portMappingCount == 0));

    for (uint32_t i = 0; i < portMappingCount; ++i)
    {
        RETURN_HR_IF(E_NOTIMPL, portMappings[i].windowsAddress != nullptr);
        RETURN_HR_IF(E_NOTIMPL, portMappings[i].protocol != 0);
    }

    internalType->ports = portMappings;
    internalType->portsCount = portMappingCount;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsVolumes(
    _In_ WslcContainerSettings* containerSettings, _In_reads_opt_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF(E_INVALIDARG, (volumes == nullptr && volumeCount != 0) || (volumes != nullptr && volumeCount == 0));

    for (uint32_t i = 0; i < volumeCount; ++i)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, volumes[i].windowsPath);
        EnsureAbsolutePath(volumes[i].windowsPath, false);
        RETURN_HR_IF_NULL(E_INVALIDARG, volumes[i].containerPath);
        EnsureAbsolutePath(volumes[i].containerPath, true);
    }

    internalType->volumes = volumes;
    internalType->volumesCount = volumeCount;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetContainerSettingsNamedVolumes(
    _In_ WslcContainerSettings* containerSettings, _In_reads_opt_(namedVolumeCount) const WslcContainerNamedVolume* namedVolumes, _In_ uint32_t namedVolumeCount)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF(E_INVALIDARG, (namedVolumes == nullptr && namedVolumeCount != 0) || (namedVolumes != nullptr && namedVolumeCount == 0));

    for (uint32_t i = 0; i < namedVolumeCount; ++i)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, namedVolumes[i].name);
        RETURN_HR_IF_NULL(E_INVALIDARG, namedVolumes[i].containerPath);
        EnsureAbsolutePath(namedVolumes[i].containerPath, true);
    }

    internalType->namedVolumes = namedVolumes;
    internalType->namedVolumesCount = namedVolumeCount;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcCreateContainerProcess(
    _In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    RETURN_HR_IF_NULL(E_POINTER, newProcess);
    *newProcess = nullptr;
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalContainer = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalContainer->container);
    auto internalProcessSettings = CheckAndGetInternalType(newProcessSettings);
    RETURN_HR_IF(E_INVALIDARG, internalProcessSettings->commandLine == nullptr || internalProcessSettings->commandLineCount == 0);

    WSLCProcessOptions runtimeOptions{};
    CopyProcessSettingsToRuntime(runtimeOptions, internalProcessSettings);

    auto result = std::make_unique<WslcProcessImpl>();
    if (SUCCEEDED(errorInfoWrapper.CaptureResult(internalContainer->container->Exec(&runtimeOptions, nullptr, &result->process))))
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->process.get());

        if (IOCallback::HasIOCallback(internalProcessSettings))
        {
            result->ioCallbacks = std::make_shared<IOCallback>(result->process.get(), internalProcessSettings->ioCallbacks);
        }

        *newProcess = reinterpret_cast<WslcProcess>(result.release());
    }

    return errorInfoWrapper;
}
CATCH_RETURN();

// GENERAL CONTAINER MANAGEMENT

STDAPI WslcGetContainerID(WslcContainer container, CHAR containerId[WSLC_CONTAINER_ID_BUFFER_SIZE])
try
{
    static_assert(WSLC_CONTAINER_ID_BUFFER_SIZE == sizeof(WSLCContainerId), "Container ID lengths differ.");

    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, containerId);

    return internalType->container->GetId(containerId);
}
CATCH_RETURN();

STDAPI WslcInspectContainer(_In_ WslcContainer container, _Outptr_result_z_ PSTR* inspectData)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, inspectData);

    *inspectData = nullptr;

    wil::unique_cotaskmem_ansistring result;
    RETURN_IF_FAILED(internalType->container->Inspect(&result));

    *inspectData = result.release();

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetContainerInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess)
try
{
    RETURN_HR_IF_NULL(E_POINTER, initProcess);
    *initProcess = nullptr;
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    auto result = std::make_unique<WslcProcessImpl>();

    RETURN_IF_FAILED(internalType->container->GetInitProcess(&result->process));

    wsl::windows::common::security::ConfigureForCOMImpersonation(result->process.get());

    result->ioCallbacks = internalType->ioCallbacks.load();

    *initProcess = reinterpret_cast<WslcProcess>(result.release());

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetContainerState(_In_ WslcContainer container, _Out_ WslcContainerState* state)
try
{
    static_assert(
        WSLC_CONTAINER_STATE_INVALID == WslcContainerStateInvalid && WSLC_CONTAINER_STATE_CREATED == WslcContainerStateCreated &&
            WSLC_CONTAINER_STATE_RUNNING == WslcContainerStateRunning &&
            WSLC_CONTAINER_STATE_EXITED == WslcContainerStateExited && WSLC_CONTAINER_STATE_DELETED == WslcContainerStateDeleted,
        "Container state enum values mismatch.");

    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, state);

    *state = WSLC_CONTAINER_STATE_INVALID;

    WSLCContainerState runtimeState{};
    RETURN_IF_FAILED(internalType->container->GetState(&runtimeState));

    *state = static_cast<WslcContainerState>(runtimeState);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcStopContainer(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutSeconds, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    return errorInfoWrapper.CaptureResult(internalType->container->Stop(Convert(signal), timeoutSeconds));
}
CATCH_RETURN();

STDAPI WslcDeleteContainer(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    return errorInfoWrapper.CaptureResult(internalType->container->Delete(ConvertFlags(flags)));
}
CATCH_RETURN();

// PROCESS DEFINITIONS

STDAPI WslcInitProcessSettings(_Out_ WslcProcessSettings* processSettings)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    *internalType = {};

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetProcessSettingsCurrentDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR currentDirectory)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    internalType->currentDirectory = currentDirectory;

    return S_OK;
}
CATCH_RETURN();

// OPTIONAL PROCESS SETTINGS

STDAPI WslcSetProcessSettingsCmdLine(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);
    RETURN_HR_IF(
        E_INVALIDARG,
        (argv == nullptr && argc != 0) || (argv != nullptr && argc == 0) ||
            (argc > static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    internalType->commandLine = argv;
    internalType->commandLineCount = static_cast<uint32_t>(argc);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSetProcessSettingsEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);
    RETURN_HR_IF(
        E_INVALIDARG,
        (key_value == nullptr && argc != 0) || (key_value != nullptr && argc == 0) ||
            (argc > static_cast<size_t>(std::numeric_limits<uint32_t>::max())));

    internalType->environment = key_value;
    internalType->environmentCount = static_cast<uint32_t>(argc);

    return S_OK;
}
CATCH_RETURN();

// PROCESS MANAGEMENT

STDAPI WslcGetProcessPid(_In_ WslcProcess process, _Out_ uint32_t* pid)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, pid);

    *pid = 0;

    int runtimePid{};
    RETURN_IF_FAILED(internalType->process->GetPid(&runtimePid));

    *pid = static_cast<uint32_t>(runtimePid);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetProcessExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, exitEvent);

    return internalType->process->GetExitEvent(exitEvent);
}
CATCH_RETURN();

// PROCESS RESULT / SIGNALS

STDAPI WslcGetProcessState(_In_ WslcProcess process, _Out_ WslcProcessState* state)
try
{
    static_assert(
        WSLC_PROCESS_STATE_UNKNOWN == WslcProcessStateUnknown && WSLC_PROCESS_STATE_RUNNING == WslcProcessStateRunning &&
            WSLC_PROCESS_STATE_EXITED == WslcProcessStateExited && WSLC_PROCESS_STATE_SIGNALLED == WslcProcessStateSignalled,
        "Process state enum values mismatch.");

    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, state);

    *state = WSLC_PROCESS_STATE_UNKNOWN;

    WSLCProcessState runtimeState{};
    int exitCode{};
    RETURN_IF_FAILED(internalType->process->GetState(&runtimeState, &exitCode));

    *state = static_cast<WslcProcessState>(runtimeState);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetProcessExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, exitCode);

    *exitCode = -1;

    WSLCProcessState runtimeState{};
    RETURN_IF_FAILED(internalType->process->GetState(&runtimeState, exitCode));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), runtimeState != WslcProcessStateExited);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSignalProcess(_In_ WslcProcess process, _In_ WslcSignal signal)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);

    RETURN_HR(internalType->process->Signal(Convert(signal)));
}
CATCH_RETURN();

STDAPI WslcSetProcessSettingsCallbacks(_In_ WslcProcessSettings* processSettings, _In_ const WslcProcessCallbacks* callbacks, _In_opt_ PVOID context)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);
    RETURN_HR_IF(E_INVALIDARG, callbacks == nullptr && context != nullptr);

    static_assert(std::is_trivial_v<WslcProcessCallbacks>, "WslcProcessCallbacks must be trivial.");

    WslcProcessCallbacks* internalCallbacks = &internalType->ioCallbacks;

    if (callbacks)
    {
        *internalCallbacks = *callbacks;
        internalType->ioCallbacks.callbackContext = context;
    }
    else
    {
        *internalCallbacks = {};
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetProcessIOHandle(_In_ WslcProcess process, _In_ WslcProcessIOHandle ioHandle, _Out_ HANDLE* handle)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, handle);

    *handle = nullptr;

    auto result = IOCallback::GetIOHandle(internalType->process.get(), ioHandle);
    *handle = result.release();

    return S_OK;
}
CATCH_RETURN();

// IMAGE MANAGEMENT
STDAPI WslcPullSessionImage(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->uri);

    auto progressCallback = ProgressCallback::CreateIf(options);

    return errorInfoWrapper.CaptureResult(internalType->session->PullImage(options->uri, options->registryAuth, progressCallback.get()));
}
CATCH_RETURN();

static HRESULT WslcImportSessionImageImpl(
    WslcSessionImpl* internalSession, PCSTR imageName, const WslcImportImageOptions* options, ErrorInfoWrapper& errorInfoWrapper, const ImageFileResolver& imageFile)
{
    auto progressCallback = ProgressCallback::CreateIf(options);

    return errorInfoWrapper.CaptureResult(internalSession->session->ImportImage(
        ToCOMInputHandle(imageFile.Handle()), imageName, progressCallback.get(), imageFile.Length()));
}

STDAPI WslcImportSessionImage(
    _In_ WslcSession session,
    _In_z_ PCSTR imageName,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentLength,
    _In_opt_ const WslcImportImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    THROW_HR_IF_NULL(E_POINTER, imageName);
    return WslcImportSessionImageImpl(internalType, imageName, options, errorInfoWrapper, {imageContent, imageContentLength});
}
CATCH_RETURN();

STDAPI WslcImportSessionImageFromFile(
    _In_ WslcSession session, _In_z_ PCSTR imageName, _In_z_ PCWSTR path, _In_opt_ const WslcImportImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    THROW_HR_IF_NULL(E_POINTER, imageName);
    return WslcImportSessionImageImpl(internalType, imageName, options, errorInfoWrapper, {path});
}
CATCH_RETURN();

static HRESULT WslcLoadSessionImageImpl(
    WslcSessionImpl* internalSession, const WslcLoadImageOptions* options, ErrorInfoWrapper& errorInfoWrapper, const ImageFileResolver& imageFile)
{
    auto progressCallback = ProgressCallback::CreateIf(options);

    return errorInfoWrapper.CaptureResult(
        internalSession->session->LoadImage(ToCOMInputHandle(imageFile.Handle()), progressCallback.get(), imageFile.Length()));
}

STDAPI WslcLoadSessionImage(
    _In_ WslcSession session,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentLength,
    _In_opt_ const WslcLoadImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    return WslcLoadSessionImageImpl(internalType, options, errorInfoWrapper, {imageContent, imageContentLength});
}
CATCH_RETURN();

STDAPI WslcLoadSessionImageFromFile(_In_ WslcSession session, _In_z_ PCWSTR path, _In_opt_ const WslcLoadImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    return WslcLoadSessionImageImpl(internalType, options, errorInfoWrapper, {path});
}
CATCH_RETURN();

STDAPI WslcDeleteSessionImage(_In_ WslcSession session, _In_z_ PCSTR NameOrId, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, NameOrId);

    WSLCDeleteImageOptions options{};
    options.Image = NameOrId;
    // TODO: Flags? (Force and NoPrune)

    wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImageInformation;

    return errorInfoWrapper.CaptureResult(
        internalType->session->DeleteImage(&options, &deletedImageInformation, deletedImageInformation.size_address<ULONG>()));
}
CATCH_RETURN();

STDAPI WslcTagSessionImage(_In_ WslcSession session, _In_ const WslcTagImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->image);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->repo);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->tag);

    WSLCTagImageOptions runtimeOptions{};
    runtimeOptions.Image = options->image;
    runtimeOptions.Repo = options->repo;
    runtimeOptions.Tag = options->tag;

    return errorInfoWrapper.CaptureResult(internalType->session->TagImage(&runtimeOptions));
}
CATCH_RETURN();

STDAPI WslcPushSessionImage(_In_ WslcSession session, _In_ const WslcPushImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->image);

    auto progressCallback = ProgressCallback::CreateIf(options);

    return errorInfoWrapper.CaptureResult(
        internalType->session->PushImage(options->image, options->registryAuth ? options->registryAuth : "", progressCallback.get()));
}
CATCH_RETURN();

STDAPI WslcSessionAuthenticate(
    _In_ WslcSession session,
    _In_z_ PCSTR serverAddress,
    _In_z_ PCSTR username,
    _In_z_ PCSTR password,
    _Outptr_result_z_ PSTR* identityToken,
    _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    ErrorInfoWrapper errorInfoWrapper{errorMessage};
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, serverAddress);
    RETURN_HR_IF_NULL(E_POINTER, username);
    RETURN_HR_IF_NULL(E_POINTER, password);
    RETURN_HR_IF_NULL(E_POINTER, identityToken);

    *identityToken = nullptr;

    wil::unique_cotaskmem_ansistring token;
    auto hr = errorInfoWrapper.CaptureResult(internalType->session->Authenticate(serverAddress, username, password, &token));
    if (SUCCEEDED(hr))
    {
        *identityToken = token.release();
    }

    return errorInfoWrapper;
}
CATCH_RETURN();

STDAPI WslcListSessionImages(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count)
try
{
    static_assert(
        sizeof(decltype(WslcImageInfo::name)) == sizeof(decltype(WSLCImageInformation::Image)), "Image name size mismatch.");

    RETURN_HR_IF_NULL(E_POINTER, images);
    *images = nullptr;
    RETURN_HR_IF_NULL(E_POINTER, count);
    *count = 0;
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);

    // TODO: Many filtering options are available via WSLC_LIST_IMAGES_OPTIONS

    wil::unique_cotaskmem_array_ptr<WSLCImageInformation> imageInformation;

    RETURN_IF_FAILED(internalType->session->ListImages(nullptr, &imageInformation, imageInformation.size_address<ULONG>()));

    if (imageInformation.size())
    {
        auto result = wil::make_unique_cotaskmem<WslcImageInfo[]>(imageInformation.size());

        for (size_t i = 0; i < imageInformation.size(); ++i)
        {
            WslcImageInfo& currentResult = result[i];
            WSLCImageInformation& currentImage = imageInformation[i];

            static_assert(std::is_trivial_v<WslcImageInfo>, "WslcImageInfo must be trivial.");
            currentResult = {};

            THROW_HR_IF(
                E_UNEXPECTED,
                memcpy_s(currentResult.name, sizeof(decltype(WslcImageInfo::name)), currentImage.Image, sizeof(decltype(WSLCImageInformation::Image))) !=
                    0);
            ConvertSHA256Hash(currentImage.Hash, currentResult.sha256);
            currentResult.sizeBytes = currentImage.Size;
            currentResult.createdTimestamp = currentImage.Created;
        }

        *images = result.release();
        *count = static_cast<uint32_t>(imageInformation.size());
    }

    return S_OK;
}
CATCH_RETURN();

// STORAGE

// INSTALL

STDAPI WslcCanRun(_Out_ BOOL* canRun, _Out_ WslcComponentFlags* missingComponents)
try
{
    RETURN_HR_IF_NULL(E_POINTER, canRun);
    RETURN_HR_IF_NULL(E_POINTER, missingComponents);

    *canRun = FALSE;
    *missingComponents = WSLC_COMPONENT_FLAG_NONE;

    WslcComponentFlags componentCheck = WSLC_COMPONENT_FLAG_NONE;

    WI_SetFlagIf(componentCheck, WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM, NeedsVirtualMachineServicesInstalled());
    WI_SetFlagIf(componentCheck, WSLC_COMPONENT_FLAG_WSL_PACKAGE, NeedsWslRuntimeInstalled());

    *canRun = componentCheck == WSLC_COMPONENT_FLAG_NONE ? TRUE : FALSE;
    *missingComponents = componentCheck;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version)
try
{
    RETURN_HR_IF_NULL(E_POINTER, version);

    static_assert(std::is_trivial_v<WslcVersion>, "WslcVersion must be trivial");
    *version = {};

    wil::com_ptr<IWSLCSessionManager> sessionManager = CreateSessionManager();

    WSLCVersion runtimeVersion{};
    RETURN_IF_FAILED(sessionManager->GetVersion(&runtimeVersion));

    version->major = runtimeVersion.Major;
    version->minor = runtimeVersion.Minor;
    version->revision = runtimeVersion.Revision;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcInstallWithDependencies(_In_opt_ WslcInstallCallback progressCallback, _In_opt_ PVOID context)
try
{
    UNREFERENCED_PARAMETER(progressCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}
CATCH_RETURN();

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        wsl::windows::common::wslutil::InitializeWil();
        WslTraceLoggingInitialize(WslcTelemetryProvider, false);
        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}
