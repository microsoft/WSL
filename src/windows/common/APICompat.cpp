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

        IFACEMETHOD(OnCrashDump)(LPCWSTR DumpPath, LPCSTR ProcessName, ULONG Pid, ULONG Signal, ULONGLONG Timestamp) override
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
    result.Type = Handle.Type;
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

WSLCCompatHandle Convert(const WSLCHandle& Handle)
{
    WSLCCompatHandle result{};
    result.Type = Handle.Type;
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
    result.Flags = Options.Flags;
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
    result.Flags = Options.Flags;
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
    result.Type = Image.Type;
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

    m_value.Flags = Options.Flags;
    m_value.StopSignal = Options.StopSignal;
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
    m_value.Flags = Options.Flags;

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
    m_value.NetworkingMode = Settings.NetworkingMode;

    m_value.FeatureFlags = Settings.FeatureFlags;
    m_value.DmesgOutput = Convert(Settings.DmesgOutput);
    m_value.StorageFlags = Settings.StorageFlags;
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
