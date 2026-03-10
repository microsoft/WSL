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

namespace {
constexpr uint32_t s_DefaultCPUCount = 2;
constexpr uint32_t s_DefaultMemoryMB = 2000;
// Maximum value per use with HVSOCKET_CONNECT_TIMEOUT_MAX
constexpr ULONG s_DefaultBootTimeout = 300000;
// Default to 1 GB
constexpr UINT64 s_DefaultStorageSize = 1000 * 1000 * 1000;

#define WSLC_FLAG_VALUE_ASSERT(_wlsc_name_, _wsla_name_) \
    static_assert(_wlsc_name_ == _wsla_name_, "Flag values differ: " #_wlsc_name_ " != " #_wsla_name_);

template <typename Flags>
struct FlagsTraits
{
    static_assert(false, "Flags used without traits defined.");
};

template <>
struct FlagsTraits<WslcSessionFeatureFlags>
{
    using WslaType = WSLAFeatureFlags;
    constexpr static WslcSessionFeatureFlags Mask = WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU;
    WSLC_FLAG_VALUE_ASSERT(WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU, WslaFeatureFlagsGPU);
};

template <>
struct FlagsTraits<WslcContainerFlags>
{
    using WslaType = WSLAContainerFlags;
    constexpr static WslcContainerFlags Mask = WSLC_CONTAINER_FLAG_AUTO_REMOVE | WSLC_CONTAINER_FLAG_ENABLE_GPU;
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_FLAG_AUTO_REMOVE, WSLAContainerFlagsRm);
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_FLAG_ENABLE_GPU, WSLAContainerFlagsGpu);
    // TODO: WSLC_CONTAINER_FLAG_PRIVILEGED has no associated runtime value
};

template <>
struct FlagsTraits<WslcContainerStartFlags>
{
    using WslaType = WSLAContainerStartFlags;
    constexpr static WslcContainerStartFlags Mask = WSLC_CONTAINER_START_FLAG_ATTACH;
    WSLC_FLAG_VALUE_ASSERT(WSLC_CONTAINER_START_FLAG_ATTACH, WSLAContainerStartFlagsAttach);
};

template <>
struct FlagsTraits<WslcDeleteContainerFlags>
{
    using WslaType = WSLADeleteFlags;
    constexpr static WslcDeleteContainerFlags Mask = WSLC_DELETE_CONTAINER_FLAG_FORCE;
    WSLC_FLAG_VALUE_ASSERT(WSLC_DELETE_CONTAINER_FLAG_FORCE, WSLADeleteFlagsForce);
};

template <typename Flags>
typename FlagsTraits<Flags>::WslaType ConvertFlags(Flags flags)
{
    using traits = FlagsTraits<Flags>;
    return static_cast<typename traits::WslaType>(flags & traits::Mask);
}

WSLASignal Convert(WslcSignal signal)
{
    switch (signal)
    {
    case WSLC_SIGNAL_NONE:
        return WSLASignal::WSLASignalNone;
    case WSLC_SIGNAL_SIGHUP:
        return WSLASignal::WSLASignalSIGHUP;
    case WSLC_SIGNAL_SIGINT:
        return WSLASignal::WSLASignalSIGINT;
    case WSLC_SIGNAL_SIGQUIT:
        return WSLASignal::WSLASignalSIGQUIT;
    case WSLC_SIGNAL_SIGKILL:
        return WSLASignal::WSLASignalSIGKILL;
    case WSLC_SIGNAL_SIGTERM:
        return WSLASignal::WSLASignalSIGTERM;
    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid WslcSignal: %i", signal);
    }
}

WSLAContainerNetworkType Convert(WslcContainerNetworkingMode mode)
{
    switch (mode)
    {
    case WSLC_CONTAINER_NETWORKING_MODE_NONE:
        return WSLAContainerNetworkTypeNone;
    case WSLC_CONTAINER_NETWORKING_MODE_BRIDGED:
        return WSLAContainerNetworkTypeBridged;
    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid WslcContainerNetworkingMode: %i", mode);
    }
}

void ConvertSHA256Hash(const char* hashString, uint8_t sha256[32])
{
    static constexpr std::string_view s_sha256Prefix = "sha256:"sv;
    static constexpr size_t s_sha256ByteCount = 32;

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

void GetErrorInfoFromCOM(PWSTR* errorMessage)
{
    if (errorMessage)
    {
        *errorMessage = nullptr;
        auto errorInfo = wsl::windows::common::wslutil::GetCOMErrorInfo();
        if (errorInfo)
        {
            *errorMessage = wil::make_unique_string<wil::unique_cotaskmem_string>(errorInfo->Message.get()).release();
        }
    }
}

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

bool CopyProcessSettingsToRuntime(WSLAProcessOptions& runtimeOptions, const WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* initProcessOptions)
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
} // namespace

// SESSION DEFINITIONS
STDAPI WslcSessionInitSettings(_In_ PCWSTR name, _In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings)
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

STDAPI WslcSessionSettingsSetCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount)
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

STDAPI WslcSessionSettingsSetMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMb)
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

STDAPI WslcSessionCreate(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session)
try
{
    RETURN_HR_IF_NULL(E_POINTER, session);
    *session = nullptr;

    auto internalType = CheckAndGetInternalType(sessionSettings);

    wil::com_ptr<IWSLASessionManager> sessionManager;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    auto result = std::make_unique<WslcSessionImpl>();
    WSLASessionSettings runtimeSettings{};
    runtimeSettings.DisplayName = internalType->displayName;
    runtimeSettings.StoragePath = internalType->storagePath;
    // TODO: Is this the intended use for vhdRequirements.sizeInBytes?
    runtimeSettings.MaximumStorageSizeMb = internalType->vhdRequirements.sizeInBytes / _1MB;
    runtimeSettings.CpuCount = internalType->cpuCount;
    runtimeSettings.MemoryMb = internalType->memoryMb;
    runtimeSettings.BootTimeoutMs = internalType->timeoutMS;
    runtimeSettings.NetworkingMode = WSLANetworkingModeVirtioProxy;
    auto terminationCallback = TerminationCallback::CreateIf(internalType);
    if (terminationCallback)
    {
        result->terminationCallback.attach(terminationCallback.as<ITerminationCallback>().detach());
        runtimeSettings.TerminationCallback = terminationCallback.get();
    }
    runtimeSettings.FeatureFlags = ConvertFlags(internalType->featureFlags);

    // TODO: Debug message output? No user control? Expects a handle value as a ULONG (to write debug info to?)
    // runtimeSettings.DmesgOutput;

    // TODO: VHD overrides; I'm not sure if we intend these to be provided.
    // runtimeSettings.RootVhdOverride = internalType->vhdRequirements.path;
    // TODO: I don't think that this VHD type override can be reused from the VHD requirements type
    //       Tracking the code suggests that this is the `filesystemtype` to the linux `mount` function.
    //       Not clear how to map dynamic and fixed to values like `ext4` and `tmpfs`.
    // runtimeSettings.RootVhdTypeOverride = ConvertType(internalType->vhdRequirements.type);

    RETURN_IF_FAILED(sessionManager->CreateSession(&runtimeSettings, WSLASessionFlagsNone, &result->session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(result->session.get());

    *session = reinterpret_cast<WslcSession>(result.release());
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionTerminate(_In_ WslcSession session)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);

    RETURN_HR(internalType->session->Terminate());
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS)
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

STDAPI WslcSessionCreateVhd(_In_ WslcSession session, _In_ const WslcVhdRequirements* options)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetVHD(_In_ WslcSessionSettings* sessionSettings, _In_ const WslcVhdRequirements* vhdRequirements)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (vhdRequirements)
    {
        internalType->vhdRequirements = *vhdRequirements;
    }
    else
    {
        internalType->vhdRequirements = {};
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    internalType->featureFlags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetTerminateCallback(
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

STDAPI WslcSessionRelease(_In_ WslcSession session)
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

STDAPI WslcContainerRelease(_In_ WslcContainer container)
try
{
    CheckAndGetInternalTypeUniquePointer(container);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessRelease(_In_ WslcProcess process)
try
{
    CheckAndGetInternalTypeUniquePointer(process);

    return S_OK;
}
CATCH_RETURN();

// CONTAINER DEFINITIONS

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF_NULL(E_POINTER, imageName);

    *internalType = {};

    internalType->image = imageName;
    // Default network configuration to WSLC SDK `0`, which is NONE.
    internalType->networking = WSLAContainerNetworkTypeNone;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerCreate(_In_ WslcSession session, _In_ const WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    RETURN_HR_IF_NULL(E_POINTER, container);
    *container = nullptr;

    auto internalSession = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalSession->session);
    auto internalContainerSettings = CheckAndGetInternalType(containerSettings);

    auto result = std::make_unique<WslcContainerImpl>();

    WSLAContainerOptions containerOptions{};
    containerOptions.Image = internalContainerSettings->image;
    containerOptions.Name = internalContainerSettings->runtimeName;
    containerOptions.HostName = internalContainerSettings->HostName;
    containerOptions.DomainName = internalContainerSettings->DomainName;
    containerOptions.Flags = ConvertFlags(internalContainerSettings->containerFlags);

    CopyProcessSettingsToRuntime(containerOptions.InitProcessOptions, internalContainerSettings->initProcessOptions);

    std::unique_ptr<WSLAVolume[]> convertedVolumes;
    if (internalContainerSettings->volumes && internalContainerSettings->volumesCount)
    {
        convertedVolumes = std::make_unique<WSLAVolume[]>(internalContainerSettings->volumesCount);
        for (uint32_t i = 0; i < internalContainerSettings->volumesCount; ++i)
        {
            const WslcContainerVolume& internalVolume = internalContainerSettings->volumes[i];
            WSLAVolume& convertedVolume = convertedVolumes[i];

            convertedVolume.HostPath = internalVolume.windowsPath;
            convertedVolume.ContainerPath = internalVolume.containerPath;
            convertedVolume.ReadOnly = internalVolume.readOnly;
        }
        containerOptions.Volumes = convertedVolumes.get();
        containerOptions.VolumesCount = static_cast<ULONG>(internalContainerSettings->volumesCount);
    }

    std::unique_ptr<WSLAPortMapping[]> convertedPorts;
    if (internalContainerSettings->ports && internalContainerSettings->portsCount)
    {
        convertedPorts = std::make_unique<WSLAPortMapping[]>(internalContainerSettings->portsCount);
        for (uint32_t i = 0; i < internalContainerSettings->portsCount; ++i)
        {
            const WslcContainerPortMapping& internalPort = internalContainerSettings->ports[i];
            WSLAPortMapping& convertedPort = convertedPorts[i];

            convertedPort.HostPort = internalPort.windowsPort;
            convertedPort.ContainerPort = internalPort.containerPort;
            // TODO: Only other supported value right now is AF_INET6; no user access.
            convertedPort.Family = AF_INET;

            // TODO: Unused protocol?
            // TODO: Unused windowsAddress?
        }
        containerOptions.Ports = convertedPorts.get();
        containerOptions.PortsCount = static_cast<ULONG>(internalContainerSettings->portsCount);
    }

    containerOptions.ContainerNetwork.ContainerNetworkType = internalContainerSettings->networking;

    // TODO: No user access
    // containerOptions.Entrypoint;
    // containerOptions.Labels;
    // containerOptions.LabelsCount;
    // containerOptions.StopSignal;
    // containerOptions.ShmSize;

    HRESULT hr = internalSession->session->CreateContainer(&containerOptions, &result->container);

    if (FAILED_LOG(hr))
    {
        GetErrorInfoFromCOM(errorMessage);
    }
    else
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->container.get());
        *container = reinterpret_cast<WslcContainer>(result.release());
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcContainerStart(_In_ WslcContainer container, _In_ WslcContainerStartFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    RETURN_HR(internalType->container->Start(ConvertFlags(flags), nullptr));
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->containerFlags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->runtimeName = name;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->HostName = hostName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->DomainName = domainName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->initProcessOptions = GetInternalType(initProcess);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->networking = Convert(networkingMode);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetPortMapping(
    _In_ WslcContainerSettings* containerSettings, _In_reads_(portMappingCount) const WslcContainerPortMapping* portMappings, _In_ uint32_t portMappingCount)
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

STDAPI WslcContainerSettingsSetVolumes(
    _In_ WslcContainerSettings* containerSettings, _In_reads_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount)
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

STDAPI WslcContainerExec(_In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess)
try
{
    auto internalContainer = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalContainer->container);
    auto internalProcessSettings = CheckAndGetInternalType(newProcessSettings);
    RETURN_HR_IF(E_INVALIDARG, internalProcessSettings->commandLine == nullptr || internalProcessSettings->commandLineCount == 0);
    RETURN_HR_IF_NULL(E_POINTER, newProcess);

    WSLAProcessOptions runtimeOptions{};
    CopyProcessSettingsToRuntime(runtimeOptions, internalProcessSettings);

    auto result = std::make_unique<WslcProcessImpl>();
    HRESULT hr = internalContainer->container->Exec(&runtimeOptions, nullptr, &result->process);

    if (FAILED_LOG(hr))
    {
        // TODO: Expected error message changes
        // GetErrorInfoFromCOM(errorMessage);
    }
    else
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->process.get());
        *newProcess = reinterpret_cast<WslcProcess>(result.release());
    }

    return hr;
}
CATCH_RETURN();

// GENERAL CONTAINER MANAGEMENT

STDAPI WslcContainerGetID(WslcContainer container, CHAR containerId[WSLC_CONTAINER_ID_LENGTH])
try
{
    static_assert(WSLC_CONTAINER_ID_LENGTH == sizeof(WSLAContainerId), "Container ID lengths differ.");

    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, containerId);

    return internalType->container->GetId(containerId);
}
CATCH_RETURN();

STDAPI WslcContainerInspect(_In_ WslcContainer container, _Outptr_result_z_ PCSTR* inspectData)
try
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(inspectData);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, initProcess);

    *initProcess = nullptr;

    auto result = std::make_unique<WslcProcessImpl>();

    HRESULT hr = internalType->container->GetInitProcess(&result->process);

    if (FAILED_LOG(hr))
    {
        // TODO: Expected error message changes
        // GetErrorInfoFromCOM(errorMessage);
    }
    else
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->process.get());
        *initProcess = reinterpret_cast<WslcProcess>(result.release());
    }

    return hr;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerGetState(_In_ WslcContainer container, _Out_ WslcContainerState* state)
try
{
    static_assert(
        WSLC_CONTAINER_STATE_INVALID == WslaContainerStateInvalid && WSLC_CONTAINER_STATE_CREATED == WslaContainerStateCreated &&
            WSLC_CONTAINER_STATE_RUNNING == WslaContainerStateRunning &&
            WSLC_CONTAINER_STATE_EXITED == WslaContainerStateExited && WSLC_CONTAINER_STATE_DELETED == WslaContainerStateDeleted,
        "Container state enum values mismatch.");

    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, state);

    *state = WSLC_CONTAINER_STATE_INVALID;

    WSLAContainerState runtimeState{};
    RETURN_IF_FAILED(internalType->container->GetState(&runtimeState));

    *state = static_cast<WslcContainerState>(runtimeState);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerStop(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutSeconds)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    RETURN_HR(internalType->container->Stop(Convert(signal), timeoutSeconds));
}
CATCH_RETURN();

STDAPI WslcContainerDelete(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    RETURN_HR(internalType->container->Delete(ConvertFlags(flags)));
}
CATCH_RETURN();

// PROCESS DEFINITIONS

STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    *internalType = {};

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessSettingsSetCurrentDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR currentDirectory)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    internalType->currentDirectory = currentDirectory;

    return S_OK;
}
CATCH_RETURN();

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsSetCmdLineArgs(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc)
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

STDAPI WslcProcessSettingsSetEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc)
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

STDAPI WslcProcessGetPid(_In_ WslcProcess process, _Out_ uint32_t* pid)
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

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, exitEvent);

    return internalType->process->GetExitEvent(exitEvent);
}
CATCH_RETURN();

// PROCESS RESULT / SIGNALS

STDAPI WslcProcessGetState(_In_ WslcProcess process, _Out_ WslcProcessState* state)
try
{
    static_assert(
        WSLC_PROCESS_STATE_UNKNOWN == WslaProcessStateUnknown && WSLC_PROCESS_STATE_RUNNING == WslaProcessStateRunning &&
            WSLC_PROCESS_STATE_EXITED == WslaProcessStateExited && WSLC_PROCESS_STATE_SIGNALLED == WslaProcessStateSignalled,
        "Process state enum values mismatch.");

    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, state);

    *state = WSLC_PROCESS_STATE_UNKNOWN;

    WSLAProcessState runtimeState{};
    int exitCode{};
    RETURN_IF_FAILED(internalType->process->GetState(&runtimeState, &exitCode));

    *state = static_cast<WslcProcessState>(runtimeState);
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, exitCode);

    *exitCode = -1;

    WSLAProcessState runtimeState{};
    RETURN_HR(internalType->process->GetState(&runtimeState, exitCode));
}
CATCH_RETURN();

STDAPI WslcProcessSignal(_In_ WslcProcess process, _In_ WslcSignal signal)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);

    RETURN_HR(internalType->process->Signal(Convert(signal)));
}
CATCH_RETURN();

STDAPI WslcProcessSettingsSetIoCallback(
    _In_ WslcProcessSettings* processSettings, _In_ WslcProcessIoHandle ioHandle, _In_opt_ WslcStdIOCallback stdIOCallback, _In_opt_ PVOID context)
try
{
    UNREFERENCED_PARAMETER(processSettings);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(stdIOCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process, _In_ WslcProcessIoHandle ioHandle, _Out_ HANDLE* handle)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, handle);

    *handle = nullptr;

    ULONG ulongHandle = 0;

    HRESULT hr = internalType->process->GetStdHandle(
        static_cast<ULONG>(static_cast<std::underlying_type_t<WslcProcessIoHandle>>(ioHandle)), &ulongHandle);

    if (SUCCEEDED_LOG(hr))
    {
        *handle = ULongToHandle(ulongHandle);
    }

    return hr;
}
CATCH_RETURN();

// IMAGE MANAGEMENT
STDAPI WslcSessionImagePull(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->uri);

    auto progressCallback = ProgressCallback::CreateIf(options);

    // TODO: Auth
    HRESULT hr = internalType->session->PullImage(options->uri, nullptr, progressCallback.get());

    if (FAILED_LOG(hr))
    {
        GetErrorInfoFromCOM(errorMessage);
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcSessionImageImport(_In_ WslcSession session, _In_ const WslcImportImageOptions* options)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionImageLoad(_In_ WslcSession session, _In_ const WslcLoadImageOptions* options)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF(E_INVALIDARG, options->ImageHandle == nullptr || options->ImageHandle == INVALID_HANDLE_VALUE);
    RETURN_HR_IF(E_INVALIDARG, options->ContentLength == 0);

    auto progressCallback = ProgressCallback::CreateIf(options);

    HRESULT hr = internalType->session->LoadImage(HandleToULong(options->ImageHandle), progressCallback.get(), options->ContentLength);

    if (FAILED_LOG(hr))
    {
        // TODO: Expected error message changes
        // GetErrorInfoFromCOM(errorMessage);
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcSessionImageDelete(_In_ WslcSession session, _In_z_ PCSTR NameOrId)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, NameOrId);

    WSLADeleteImageOptions options{};
    options.Image = NameOrId;
    // TODO: Flags? (Force and NoPrune)

    wil::unique_cotaskmem_array_ptr<WSLADeletedImageInformation> deletedImageInformation;

    RETURN_HR(internalType->session->DeleteImage(&options, &deletedImageInformation, deletedImageInformation.size_address<ULONG>()));
}
CATCH_RETURN();

STDAPI WslcSessionImageList(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count)
try
{
    static_assert(
        sizeof(decltype(WslcImageInfo::name)) == sizeof(decltype(WSLAImageInformation::Image)), "Image name size mismatch.");

    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, images);
    RETURN_HR_IF_NULL(E_POINTER, count);

    *images = nullptr;
    *count = 0;

    // TODO: Many filtering options are available via WSLA_LIST_IMAGES_OPTIONS

    wil::unique_cotaskmem_array_ptr<WSLAImageInformation> imageInformation;

    RETURN_IF_FAILED(internalType->session->ListImages(nullptr, &imageInformation, imageInformation.size_address<ULONG>()));

    if (imageInformation.size())
    {
        auto result = wil::make_unique_cotaskmem<WslcImageInfo[]>(imageInformation.size());

        for (size_t i = 0; i < imageInformation.size(); ++i)
        {
            WslcImageInfo& currentResult = result[i];
            WSLAImageInformation& currentImage = imageInformation[i];

            THROW_HR_IF(
                E_UNEXPECTED,
                memcpy_s(currentResult.name, sizeof(decltype(WslcImageInfo::name)), currentImage.Image, sizeof(decltype(WSLAImageInformation::Image))) !=
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
    UNREFERENCED_PARAMETER(canRun);
    UNREFERENCED_PARAMETER(missingComponents);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version)
try
{
    RETURN_HR_IF_NULL(E_POINTER, version);

    static_assert(std::is_trivial_v<WslcVersion>, "WslcVersion must be trivial");
    *version = {};

    // TODO: Returning the WSLC SDK API version, but do callers actually need the WSL runtime
    //       version instead as they will have built and shipped with the SDK?
    version->major = wsl::shared::VersionMajor;
    version->minor = wsl::shared::VersionMinor;
    version->revision = wsl::shared::VersionRevision;

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
