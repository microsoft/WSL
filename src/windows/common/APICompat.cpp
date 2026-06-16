// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "APICompat.h"
#include <wrl/implements.h>

namespace wsl::windows::common::apicompat {

namespace {

    template <size_t Size>
    void CopyString(char (&Destination)[Size], const char* Source)
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(Destination, Source) != 0);
    }

    // Throws E_INVALIDARG if Value contains any bit outside of KnownMask.
    template <typename TValue, typename TMask>
    void ThrowIfUnknownFlags(TValue Value, TMask KnownMask, PCSTR Name)
    {
        const auto value = static_cast<unsigned long long>(Value);
        const auto mask = static_cast<unsigned long long>(KnownMask);
        THROW_HR_IF_MSG(E_INVALIDARG, (value & ~mask) != 0, "Invalid %hs value: 0x%llx", Name, value);
    }

    template <typename TOut, typename TIn>
    std::vector<TOut> ConvertArray(const TIn* Items, ULONG Count)
    {
        std::vector<TOut> result;
        if (Items == nullptr || Count == 0)
        {
            return result;
        }

        result.reserve(Count);
        for (ULONG index = 0; index < Count; index++)
        {
            result.push_back(Convert(Items[index]));
        }

        return result;
    }

    class CrashDumpCallbackAdapter
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICrashDumpCallback>
    {
    public:
        CrashDumpCallbackAdapter(IWSLCCompatCrashDumpCallback* Inner) : m_inner(Inner)
        {
        }

        IFACEMETHOD(OnCrashDump)(LPCWSTR DumpPath, LPCSTR ProcessName, ULONGLONG Pid, ULONG Signal, ULONGLONG Timestamp) override
        {
            return m_inner->OnCrashDump(DumpPath, ProcessName, Pid, Signal, Timestamp);
        }

    private:
        Microsoft::WRL::ComPtr<IWSLCCompatCrashDumpCallback> m_inner;
    };

    class ProgressCallbackAdapter
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
    {
    public:
        ProgressCallbackAdapter(IWSLCCompatProgressCallback* Inner) : m_inner(Inner)
        {
        }

        IFACEMETHOD(OnProgress)(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
        {
            return m_inner->OnProgress(Status, Id, Current, Total);
        }

    private:
        Microsoft::WRL::ComPtr<IWSLCCompatProgressCallback> m_inner;
    };

    class WarningCallbackAdapter
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWarningCallback>
    {
    public:
        WarningCallbackAdapter(IWSLCCompatWarningCallback* Inner) : m_inner(Inner)
        {
        }

        IFACEMETHOD(OnWarning)(LPCWSTR Message) override
        {
            return m_inner->OnWarning(Message);
        }

    private:
        Microsoft::WRL::ComPtr<IWSLCCompatWarningCallback> m_inner;
    };

} // namespace

//
// Enum conversions.
//

WSLCFD Convert(WSLCCompatFD Fd)
{
    switch (Fd)
    {
    case WSLCCompatFDStdin:
        return WSLCFDStdin;
    case WSLCCompatFDStdout:
        return WSLCFDStdout;
    case WSLCCompatFDStderr:
        return WSLCFDStderr;
    case WSLCCompatFDTty:
        return WSLCFDTty;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCCompatFD value: %i", static_cast<int>(Fd));
}

WSLCSignal Convert(WSLCCompatSignal Signal)
{
    switch (Signal)
    {
    case WSLCCompatSignalNone:
        return WSLCSignalNone;
    case WSLCCompatSignalSIGHUP:
        return WSLCSignalSIGHUP;
    case WSLCCompatSignalSIGINT:
        return WSLCSignalSIGINT;
    case WSLCCompatSignalSIGQUIT:
        return WSLCSignalSIGQUIT;
    case WSLCCompatSignalSIGILL:
        return WSLCSignalSIGILL;
    case WSLCCompatSignalSIGTRAP:
        return WSLCSignalSIGTRAP;
    case WSLCCompatSignalSIGABRT:
        return WSLCSignalSIGABRT;
    case WSLCCompatSignalSIGBUS:
        return WSLCSignalSIGBUS;
    case WSLCCompatSignalSIGFPE:
        return WSLCSignalSIGFPE;
    case WSLCCompatSignalSIGKILL:
        return WSLCSignalSIGKILL;
    case WSLCCompatSignalSIGUSR1:
        return WSLCSignalSIGUSR1;
    case WSLCCompatSignalSIGSEGV:
        return WSLCSignalSIGSEGV;
    case WSLCCompatSignalSIGUSR2:
        return WSLCSignalSIGUSR2;
    case WSLCCompatSignalSIGPIPE:
        return WSLCSignalSIGPIPE;
    case WSLCCompatSignalSIGALRM:
        return WSLCSignalSIGALRM;
    case WSLCCompatSignalSIGTERM:
        return WSLCSignalSIGTERM;
    case WSLCCompatSignalSIGTKFLT:
        return WSLCSignalSIGTKFLT;
    case WSLCCompatSignalSIGCHLD:
        return WSLCSignalSIGCHLD;
    case WSLCCompatSignalSIGCONT:
        return WSLCSignalSIGCONT;
    case WSLCCompatSignalSIGSTOP:
        return WSLCSignalSIGSTOP;
    case WSLCCompatSignalSIGTSTP:
        return WSLCSignalSIGTSTP;
    case WSLCCompatSignalSIGTTIN:
        return WSLCSignalSIGTTIN;
    case WSLCCompatSignalSIGTTOU:
        return WSLCSignalSIGTTOU;
    case WSLCCompatSignalSIGURG:
        return WSLCSignalSIGURG;
    case WSLCCompatSignalSIGXCPU:
        return WSLCSignalSIGXCPU;
    case WSLCCompatSignalSIGXFSZ:
        return WSLCSignalSIGXFSZ;
    case WSLCCompatSignalSIGVTALRM:
        return WSLCSignalSIGVTALRM;
    case WSLCCompatSignalSIGPROF:
        return WSLCSignalSIGPROF;
    case WSLCCompatSignalSIGWINCH:
        return WSLCSignalSIGWINCH;
    case WSLCCompatSignalSIGIO:
        return WSLCSignalSIGIO;
    case WSLCCompatSignalSIGPWR:
        return WSLCSignalSIGPWR;
    case WSLCCompatSignalSIGSYS:
        return WSLCSignalSIGSYS;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid Signal value: %i", static_cast<int>(Signal));
}

WSLCProcessFlags Convert(WSLCCompatProcessFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatProcessFlagsStdin | WSLCCompatProcessFlagsTty, "WSLCCompatProcessFlags");

    WSLCProcessFlags result = WSLCProcessFlagsNone;
    WI_SetFlagIf(result, WSLCProcessFlagsStdin, WI_IsFlagSet(Flags, WSLCCompatProcessFlagsStdin));
    WI_SetFlagIf(result, WSLCProcessFlagsTty, WI_IsFlagSet(Flags, WSLCCompatProcessFlagsTty));
    return result;
}

WSLCContainerFlags Convert(WSLCCompatContainerFlags Flags)
{
    ThrowIfUnknownFlags(
        Flags,
        WSLCCompatContainerFlagsRm | WSLCCompatContainerFlagsGpu | WSLCCompatContainerFlagsInit | WSLCCompatContainerFlagsPublishAll,
        "WSLCCompatContainerFlags");

    WSLCContainerFlags result = WSLCContainerFlagsNone;
    WI_SetFlagIf(result, WSLCContainerFlagsRm, WI_IsFlagSet(Flags, WSLCCompatContainerFlagsRm));
    WI_SetFlagIf(result, WSLCContainerFlagsGpu, WI_IsFlagSet(Flags, WSLCCompatContainerFlagsGpu));
    WI_SetFlagIf(result, WSLCContainerFlagsInit, WI_IsFlagSet(Flags, WSLCCompatContainerFlagsInit));
    WI_SetFlagIf(result, WSLCContainerFlagsPublishAll, WI_IsFlagSet(Flags, WSLCCompatContainerFlagsPublishAll));
    return result;
}

WSLCContainerStartFlags Convert(WSLCCompatContainerStartFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatContainerStartFlagsAttach, "WSLCCompatContainerStartFlags");

    WSLCContainerStartFlags result = WSLCContainerStartFlagsNone;
    WI_SetFlagIf(result, WSLCContainerStartFlagsAttach, WI_IsFlagSet(Flags, WSLCCompatContainerStartFlagsAttach));
    return result;
}

WSLCDeleteFlags Convert(WSLCCompatDeleteFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatDeleteFlagsForce | WSLCCompatDeleteFlagsDeleteVolumes, "WSLCCompatDeleteFlags");

    WSLCDeleteFlags result = WSLCDeleteFlagsNone;
    WI_SetFlagIf(result, WSLCDeleteFlagsForce, WI_IsFlagSet(Flags, WSLCCompatDeleteFlagsForce));
    WI_SetFlagIf(result, WSLCDeleteFlagsDeleteVolumes, WI_IsFlagSet(Flags, WSLCCompatDeleteFlagsDeleteVolumes));
    return result;
}

WSLCNetworkingMode Convert(WSLCCompatNetworkingMode Mode)
{
    switch (Mode)
    {
    case WSLCCompatNetworkingModeNone:
        return WSLCNetworkingModeNone;
    case WSLCCompatNetworkingModeNAT:
        return WSLCNetworkingModeNAT;
    case WSLCCompatNetworkingModeVirtioProxy:
        return WSLCNetworkingModeVirtioProxy;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCCompatNetworkingMode value: %i", static_cast<int>(Mode));
}

WSLCFeatureFlags Convert(WSLCCompatFeatureFlags Flags)
{
    ThrowIfUnknownFlags(
        Flags,
        WSLCCompatFeatureFlagsDnsTunneling | WSLCCompatFeatureFlagsEarlyBootDmesg | WSLCCompatFeatureFlagsGPU |
            WSLCCompatFeatureFlagsVirtioFs | WSLCCompatFeatureFlagsDebug,
        "WSLCCompatFeatureFlags");

    WSLCFeatureFlags result = WslcFeatureFlagsNone;
    WI_SetFlagIf(result, WslcFeatureFlagsDnsTunneling, WI_IsFlagSet(Flags, WSLCCompatFeatureFlagsDnsTunneling));
    WI_SetFlagIf(result, WslcFeatureFlagsEarlyBootDmesg, WI_IsFlagSet(Flags, WSLCCompatFeatureFlagsEarlyBootDmesg));
    WI_SetFlagIf(result, WslcFeatureFlagsGPU, WI_IsFlagSet(Flags, WSLCCompatFeatureFlagsGPU));
    WI_SetFlagIf(result, WslcFeatureFlagsVirtioFs, WI_IsFlagSet(Flags, WSLCCompatFeatureFlagsVirtioFs));
    WI_SetFlagIf(result, WslcFeatureFlagsDebug, WI_IsFlagSet(Flags, WSLCCompatFeatureFlagsDebug));
    return result;
}

WSLCSessionStorageFlags Convert(WSLCCompatSessionStorageFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatSessionStorageFlagsNoCreate, "WSLCCompatSessionStorageFlags");

    WSLCSessionStorageFlags result = WSLCSessionStorageFlagsNone;
    WI_SetFlagIf(result, WSLCSessionStorageFlagsNoCreate, WI_IsFlagSet(Flags, WSLCCompatSessionStorageFlagsNoCreate));
    return result;
}

WSLCSessionFlags Convert(WSLCCompatSessionFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatSessionFlagsPersistent | WSLCCompatSessionFlagsOpenExisting, "WSLCCompatSessionFlags");

    WSLCSessionFlags result = WSLCSessionFlagsNone;
    WI_SetFlagIf(result, WSLCSessionFlagsPersistent, WI_IsFlagSet(Flags, WSLCCompatSessionFlagsPersistent));
    WI_SetFlagIf(result, WSLCSessionFlagsOpenExisting, WI_IsFlagSet(Flags, WSLCCompatSessionFlagsOpenExisting));
    return result;
}

WSLCListImagesFlags Convert(WSLCCompatListImagesFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatListImagesFlagsAll | WSLCCompatListImagesFlagsDigests, "WSLCCompatListImagesFlags");

    WSLCListImagesFlags result = WSLCListImagesFlagsNone;
    WI_SetFlagIf(result, WSLCListImagesFlagsAll, WI_IsFlagSet(Flags, WSLCCompatListImagesFlagsAll));
    WI_SetFlagIf(result, WSLCListImagesFlagsDigests, WI_IsFlagSet(Flags, WSLCCompatListImagesFlagsDigests));
    return result;
}

WSLCDeleteImageFlags Convert(WSLCCompatDeleteImageFlags Flags)
{
    ThrowIfUnknownFlags(Flags, WSLCCompatDeleteImageFlagsForce | WSLCCompatDeleteImageFlagsNoPrune, "WSLCCompatDeleteImageFlags");

    WSLCDeleteImageFlags result = WSLCDeleteImageFlagsNone;
    WI_SetFlagIf(result, WSLCDeleteImageFlagsForce, WI_IsFlagSet(Flags, WSLCCompatDeleteImageFlagsForce));
    WI_SetFlagIf(result, WSLCDeleteImageFlagsNoPrune, WI_IsFlagSet(Flags, WSLCCompatDeleteImageFlagsNoPrune));
    return result;
}

WSLCHandleType Convert(WSLCCompatHandleType Type)
{
    switch (Type)
    {
    case WSLCCompatHandleTypeUnknown:
        return WSLCHandleTypeUnknown;
    case WSLCCompatHandleTypeFile:
        return WSLCHandleTypeFile;
    case WSLCCompatHandleTypePipe:
        return WSLCHandleTypePipe;
    case WSLCCompatHandleTypeSocket:
        return WSLCHandleTypeSocket;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCCompatHandleType value: %i", static_cast<int>(Type));
}

WSLCCompatProcessState Convert(WSLCProcessState State)
{
    switch (State)
    {
    case WslcProcessStateUnknown:
        return WSLCCompatProcessStateUnknown;
    case WslcProcessStateRunning:
        return WSLCCompatProcessStateRunning;
    case WslcProcessStateExited:
        return WSLCCompatProcessStateExited;
    case WslcProcessStateSignalled:
        return WSLCCompatProcessStateSignalled;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCProcessState value: %i", static_cast<int>(State));
}

WSLCCompatContainerState Convert(WSLCContainerState State)
{
    switch (State)
    {
    case WslcContainerStateInvalid:
        return WSLCCompatContainerStateInvalid;
    case WslcContainerStateCreated:
        return WSLCCompatContainerStateCreated;
    case WslcContainerStateRunning:
        return WSLCCompatContainerStateRunning;
    case WslcContainerStateExited:
        return WSLCCompatContainerStateExited;
    case WslcContainerStateDeleted:
        return WSLCCompatContainerStateDeleted;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCContainerState value: %i", static_cast<int>(State));
}

WSLCCompatVirtualMachineTerminationReason Convert(WSLCVirtualMachineTerminationReason Reason)
{
    switch (Reason)
    {
    case WSLCVirtualMachineTerminationReasonUnknown:
        return WSLCCompatVirtualMachineTerminationReasonUnknown;
    case WSLCVirtualMachineTerminationReasonShutdown:
        return WSLCCompatVirtualMachineTerminationReasonShutdown;
    case WSLCVirtualMachineTerminationReasonCrashed:
        return WSLCCompatVirtualMachineTerminationReasonCrashed;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCVirtualMachineTerminationReason value: %i", static_cast<int>(Reason));
}

WSLCCompatHandleType Convert(WSLCHandleType Type)
{
    switch (Type)
    {
    case WSLCHandleTypeUnknown:
        return WSLCCompatHandleTypeUnknown;
    case WSLCHandleTypeFile:
        return WSLCCompatHandleTypeFile;
    case WSLCHandleTypePipe:
        return WSLCCompatHandleTypePipe;
    case WSLCHandleTypeSocket:
        return WSLCCompatHandleTypeSocket;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCHandleType value: %i", static_cast<int>(Type));
}

WSLCCompatDeletedImageType Convert(WSLCDeletedImageType Type)
{
    switch (Type)
    {
    case WSLCDeletedImageTypeDeleted:
        return WSLCCompatDeletedImageTypeDeleted;
    case WSLCDeletedImageTypeUntagged:
        return WSLCCompatDeletedImageTypeUntagged;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid WSLCDeletedImageType value: %i", static_cast<int>(Type));
}

//
// Scalar / non-owning struct conversions.
//

WSLCVersion Convert(const WSLCCompatVersion& Version)
{
    WSLCVersion result{};
    result.Major = Version.Major;
    result.Minor = Version.Minor;
    result.Revision = Version.Revision;
    return result;
}

WSLCCompatVersion Convert(const WSLCVersion& Version)
{
    WSLCCompatVersion result{};
    result.Major = Version.Major;
    result.Minor = Version.Minor;
    result.Revision = Version.Revision;
    return result;
}

WSLCHandle Convert(const WSLCCompatHandle& Handle)
{
    WSLCHandle result{};
    result.Type = Convert(Handle.Type);
    switch (Handle.Type)
    {
    case WSLCCompatHandleTypeFile:
        result.Handle.File = Handle.Handle.File;
        break;
    case WSLCCompatHandleTypePipe:
        result.Handle.Pipe = Handle.Handle.Pipe;
        break;
    case WSLCCompatHandleTypeSocket:
        result.Handle.Socket = Handle.Handle.Socket;
        break;
    case WSLCCompatHandleTypeUnknown:
        break;
    }

    return result;
}

WSLCCompatHandle Convert(const WSLCHandle& Handle)
{
    WSLCCompatHandle result{};
    result.Type = Convert(Handle.Type);
    switch (Handle.Type)
    {
    case WSLCHandleTypeFile:
        result.Handle.File = Handle.Handle.File;
        break;
    case WSLCHandleTypePipe:
        result.Handle.Pipe = Handle.Handle.Pipe;
        break;
    case WSLCHandleTypeSocket:
        result.Handle.Socket = Handle.Handle.Socket;
        break;
    case WSLCHandleTypeUnknown:
        break;
    }

    return result;
}

WSLCStringArray Convert(const WSLCCompatStringArray& Array)
{
    WSLCStringArray result{};
    result.Values = Array.Values;
    result.Count = Array.Count;
    return result;
}

WSLCProcessOptions Convert(const WSLCCompatProcessOptions& Options)
{
    WSLCProcessOptions result{};
    result.CurrentDirectory = Options.CurrentDirectory;
    result.User = Options.User;
    result.CommandLine = Convert(Options.CommandLine);
    result.Environment = Convert(Options.Environment);
    result.Flags = Convert(Options.Flags);
    return result;
}

KeyValuePair Convert(const WSLCCompatKeyValuePair& Pair)
{
    KeyValuePair result{};
    result.Key = Pair.Key;
    result.Value = Pair.Value;
    return result;
}

WSLCVolume Convert(const WSLCCompatVolume& Volume)
{
    WSLCVolume result{};
    result.HostPath = Volume.HostPath;
    result.ContainerPath = Volume.ContainerPath;
    result.ReadOnly = Volume.ReadOnly;
    return result;
}

WSLCNamedVolume Convert(const WSLCCompatNamedVolume& Volume)
{
    WSLCNamedVolume result{};
    result.Name = Volume.Name;
    result.ContainerPath = Volume.ContainerPath;
    result.ReadOnly = Volume.ReadOnly;
    return result;
}

WSLCPortMapping Convert(const WSLCCompatPortMapping& Port)
{
    WSLCPortMapping result{};
    result.HostPort = Port.HostPort;
    result.ContainerPort = Port.ContainerPort;
    result.Family = Port.Family;
    result.Protocol = Port.Protocol;
    CopyString(result.BindingAddress, Port.BindingAddress);
    return result;
}

WSLCTmpfsMount Convert(const WSLCCompatTmpfsMount& Mount)
{
    WSLCTmpfsMount result{};
    result.Destination = Mount.Destination;
    result.Options = Mount.Options;
    return result;
}

WSLCUlimit Convert(const WSLCCompatUlimit& Ulimit)
{
    WSLCUlimit result{};
    result.Name = Ulimit.Name;
    result.Soft = Ulimit.Soft;
    result.Hard = Ulimit.Hard;
    return result;
}

WSLCDeleteImageOptions Convert(const WSLCCompatDeleteImageOptions& Options)
{
    WSLCDeleteImageOptions result{};
    result.Image = Options.Image;
    result.Flags = static_cast<DWORD>(Convert(static_cast<WSLCCompatDeleteImageFlags>(Options.Flags)));
    return result;
}

WSLCTagImageOptions Convert(const WSLCCompatTagImageOptions& Options)
{
    WSLCTagImageOptions result{};
    result.Image = Options.Image;
    result.Repo = Options.Repo;
    result.Tag = Options.Tag;
    return result;
}

WSLCCompatImageInformation Convert(const WSLCImageInformation& Image)
{
    WSLCCompatImageInformation result{};
    CopyString(result.Image, Image.Image);
    CopyString(result.Hash, Image.Hash);
    CopyString(result.Digest, Image.Digest);
    result.Size = Image.Size;
    result.Created = Image.Created;
    CopyString(result.ParentId, Image.ParentId);
    return result;
}

WSLCCompatDeletedImageInformation Convert(const WSLCDeletedImageInformation& Image)
{
    WSLCCompatDeletedImageInformation result{};
    CopyString(result.Image, Image.Image);
    result.Type = Convert(Image.Type);
    return result;
}

WSLCCompatVolumeInformation Convert(const WSLCVolumeInformation& Volume)
{
    WSLCCompatVolumeInformation result{};
    CopyString(result.Name, Volume.Name);
    CopyString(result.Driver, Volume.Driver);
    return result;
}

//
// Composite struct conversions.
//

ContainerOptionsConversion::ContainerOptionsConversion(const WSLCCompatContainerOptions& Options)
{
    m_value.Image = Options.Image;
    m_value.Name = Options.Name;
    m_value.Entrypoint = Convert(Options.Entrypoint);
    m_value.InitProcessOptions = Convert(Options.InitProcessOptions);

    m_volumes = ConvertArray<WSLCVolume>(Options.Volumes, Options.VolumesCount);
    m_value.Volumes = m_volumes.empty() ? nullptr : m_volumes.data();
    m_value.VolumesCount = Options.VolumesCount;

    m_ports = ConvertArray<WSLCPortMapping>(Options.Ports, Options.PortsCount);
    m_value.Ports = m_ports.empty() ? nullptr : m_ports.data();
    m_value.PortsCount = Options.PortsCount;

    m_labels = ConvertArray<WSLCLabel>(Options.Labels, Options.LabelsCount);
    m_value.Labels = m_labels.empty() ? nullptr : m_labels.data();
    m_value.LabelsCount = Options.LabelsCount;

    m_value.Flags = Convert(Options.Flags);
    m_value.StopSignal = Convert(Options.StopSignal);
    m_value.HostName = Options.HostName;
    m_value.DomainName = Options.DomainName;

    m_value.DnsServers = Convert(Options.DnsServers);
    m_value.DnsSearchDomains = Convert(Options.DnsSearchDomains);
    m_value.DnsOptions = Convert(Options.DnsOptions);

    m_value.ShmSize = Options.ShmSize;

    // Container network (nested arrays whose elements have their own arrays).
    m_value.ContainerNetwork.NetworkMode = Options.ContainerNetwork.NetworkMode;
    const ULONG networksCount = Options.ContainerNetwork.NetworksCount;
    THROW_HR_IF(E_INVALIDARG, networksCount > 0 && Options.ContainerNetwork.Networks == nullptr);

    m_networks.reserve(networksCount);
    m_networkSettings.reserve(networksCount);
    for (ULONG index = 0; index < networksCount; index++)
    {
        const auto& network = Options.ContainerNetwork.Networks[index];
        THROW_HR_IF(E_INVALIDARG, network.SettingsCount > 0 && network.Settings == nullptr);

        m_networkSettings.push_back(ConvertArray<KeyValuePair>(network.Settings, network.SettingsCount));

        WSLCNetworkConnection connection{};
        connection.NetworkName = network.NetworkName;
        connection.Settings = m_networkSettings.back().empty() ? nullptr : m_networkSettings.back().data();
        connection.SettingsCount = network.SettingsCount;
        m_networks.push_back(connection);
    }

    m_value.ContainerNetwork.Networks = m_networks.empty() ? nullptr : m_networks.data();
    m_value.ContainerNetwork.NetworksCount = networksCount;

    m_tmpfs = ConvertArray<WSLCTmpfsMount>(Options.Tmpfs, Options.TmpfsCount);
    m_value.Tmpfs = m_tmpfs.empty() ? nullptr : m_tmpfs.data();
    m_value.TmpfsCount = Options.TmpfsCount;

    m_namedVolumes = ConvertArray<WSLCNamedVolume>(Options.NamedVolumes, Options.NamedVolumesCount);
    m_value.NamedVolumes = m_namedVolumes.empty() ? nullptr : m_namedVolumes.data();
    m_value.NamedVolumesCount = Options.NamedVolumesCount;

    m_value.MemoryBytes = Options.MemoryBytes;
    m_value.NanoCpus = Options.NanoCpus;

    m_ulimits = ConvertArray<WSLCUlimit>(Options.Ulimits, Options.UlimitsCount);
    m_value.Ulimits = m_ulimits.empty() ? nullptr : m_ulimits.data();
    m_value.UlimitsCount = Options.UlimitsCount;
}

ListImagesOptionsConversion::ListImagesOptionsConversion(const WSLCCompatListImagesOptions& Options)
{
    m_value.Flags = static_cast<DWORD>(Convert(static_cast<WSLCCompatListImagesFlags>(Options.Flags)));

    m_filters = ConvertArray<WSLCFilter>(Options.Filters, Options.FiltersCount);
    m_value.Filters = m_filters.empty() ? nullptr : m_filters.data();
    m_value.FiltersCount = Options.FiltersCount;
}

VolumeOptionsConversion::VolumeOptionsConversion(const WSLCCompatVolumeOptions& Options)
{
    m_value.Name = Options.Name;
    m_value.Driver = Options.Driver;

    m_driverOpts = ConvertArray<WSLCDriverOption>(Options.DriverOpts, Options.DriverOptsCount);
    m_value.DriverOpts = m_driverOpts.empty() ? nullptr : m_driverOpts.data();
    m_value.DriverOptsCount = Options.DriverOptsCount;

    m_labels = ConvertArray<WSLCLabel>(Options.Labels, Options.LabelsCount);
    m_value.Labels = m_labels.empty() ? nullptr : m_labels.data();
    m_value.LabelsCount = Options.LabelsCount;
}

SessionSettingsConversion::SessionSettingsConversion(const WSLCCompatSessionSettings& Settings)
{
    m_value.DisplayName = Settings.DisplayName;
    m_value.StoragePath = Settings.StoragePath;
    m_value.MaximumStorageSizeMb = Settings.MaximumStorageSizeMb;
    m_value.CpuCount = Settings.CpuCount;
    m_value.MemoryMb = Settings.MemoryMb;
    m_value.BootTimeoutMs = Settings.BootTimeoutMs;
    m_value.NetworkingMode = Convert(Settings.NetworkingMode);

    m_value.FeatureFlags = Convert(Settings.FeatureFlags);
    m_value.DmesgOutput = Convert(Settings.DmesgOutput);
    m_value.StorageFlags = Convert(Settings.StorageFlags);
    m_value.RootVhdOverride = Settings.RootVhdOverride;
    m_value.RootVhdTypeOverride = Settings.RootVhdTypeOverride;
}

//
// Callback conversions.
//

Microsoft::WRL::ComPtr<ICrashDumpCallback> Convert(IWSLCCompatCrashDumpCallback* Callback)
{
    Microsoft::WRL::ComPtr<ICrashDumpCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<CrashDumpCallbackAdapter>(Callback);
    }

    return result;
}

Microsoft::WRL::ComPtr<IProgressCallback> Convert(IWSLCCompatProgressCallback* Callback)
{
    Microsoft::WRL::ComPtr<IProgressCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<ProgressCallbackAdapter>(Callback);
    }

    return result;
}

Microsoft::WRL::ComPtr<IWarningCallback> Convert(IWSLCCompatWarningCallback* Callback)
{
    Microsoft::WRL::ComPtr<IWarningCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<WarningCallbackAdapter>(Callback);
    }

    return result;
}

} // namespace wsl::windows::common::apicompat
