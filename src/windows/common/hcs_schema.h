/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hcs_schema.h

Abstract:

    This file contains the host compute service schema definitions.

--*/

#pragma once

#include "JsonUtils.h"

#define OMIT_IF_EMPTY(Json, Object, Value) \
    if ((Object).Value.has_value()) \
    { \
        Json[#Value] = (Object).Value.value(); \
    }

#define ASSIGN_IF_PRESENT(Json, Object, Value) \
    if (Json.contains(#Value)) \
    { \
        (Object).Value = Json.at(#Value).get_to((Object).Value); \
    }

namespace wsl::windows::common::hcs {

enum class ModifyRequestType
{
    Add,
    Update,
    Remove
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ModifyRequestType,
    {
        {ModifyRequestType::Add, "Add"},
        {ModifyRequestType::Update, "Update"},
        {ModifyRequestType::Remove, "Remove"},
    })

enum class AttachmentType
{
    VirtualDisk,
    PassThru
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    AttachmentType,
    {
        {AttachmentType::VirtualDisk, "VirtualDisk"},
        {AttachmentType::PassThru, "PassThru"},
    })

enum class PropertyType
{
    Basic
};

NLOHMANN_JSON_SERIALIZE_ENUM(PropertyType, {{PropertyType::Basic, "Basic"}})

struct Attachment
{
    Attachment() = default;
    AttachmentType Type;
    std::wstring Path;
    bool ReadOnly;
    bool SupportCompressedVolumes;
    bool AlwaysAllowSparseFiles;
    bool SupportEncryptedFiles;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Attachment, Type, Path, ReadOnly, SupportCompressedVolumes, AlwaysAllowSparseFiles, SupportEncryptedFiles);
};

enum class Plan9ShareFlags
{
    None = 0x00000000,
    ReadOnly = 0x00000001,
    LinuxMetadata = 0x00000004,
    CaseSensitive = 0x00000008,
    UseShareRootIdentity = 0x00000010,
    AllowOptions = 0x00000020,
    AllowSubPaths = 0x00000040
};

DEFINE_ENUM_FLAG_OPERATORS(Plan9ShareFlags);

struct Plan9Share
{
    std::wstring Name;
    std::wstring AccessName;
    std::wstring Path;
    uint32_t Port;
    Plan9ShareFlags Flags;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Plan9Share, Name, AccessName, Path, Port, Flags);
};

enum class FlexibleIoDeviceHostingModel
{
    ExternalRestricted
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    FlexibleIoDeviceHostingModel,
    {
        {FlexibleIoDeviceHostingModel::ExternalRestricted, "ExternalRestricted"},
    })

struct FlexibleIoDevice
{
    GUID EmulatorId;
    FlexibleIoDeviceHostingModel HostingModel;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(FlexibleIoDevice, EmulatorId, HostingModel);
};

struct NetworkAdapter
{
    GUID EndpointId;
    wsl::shared::string::MacAddress MacAddress;
    std::optional<GUID> InstanceId;
    std::optional<bool> IsConnected;
    std::optional<GUID> SwitchId;
    std::optional<GUID> PortId;
};

inline void to_json(nlohmann::json& j, const NetworkAdapter& adapter)
{
    j = nlohmann::json{{"EndpointId", adapter.EndpointId}, {"MacAddress", adapter.MacAddress}};

    OMIT_IF_EMPTY(j, adapter, InstanceId);
    OMIT_IF_EMPTY(j, adapter, IsConnected);
    OMIT_IF_EMPTY(j, adapter, SwitchId);
    OMIT_IF_EMPTY(j, adapter, PortId);
}

enum class GpuAssignmentMode
{
    Mirror
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    GpuAssignmentMode,
    {
        {GpuAssignmentMode::Mirror, "Mirror"},
    })

struct GpuConfiguration
{
    GpuAssignmentMode AssignmentMode;
    bool AllowVendorExtension;
    std::optional<bool> DisableGdiAcceleration;
    std::optional<bool> DisablePresentation;
};

inline void to_json(nlohmann::json& j, const GpuConfiguration& configuration)
{
    j = nlohmann::json{{"AssignmentMode", configuration.AssignmentMode}, {"AllowVendorExtension", configuration.AllowVendorExtension}};

    OMIT_IF_EMPTY(j, configuration, DisableGdiAcceleration);
    OMIT_IF_EMPTY(j, configuration, DisablePresentation);
}

template <typename TSettings>
struct ModifySettingRequest
{
    std::wstring ResourcePath;
    ModifyRequestType RequestType;
    TSettings Settings;
};

template <>
struct ModifySettingRequest<void>
{
    ModifySettingRequest() = default;
    std::wstring ResourcePath;
    ModifyRequestType RequestType;
};

template <typename TSettings>
inline void to_json(nlohmann::json& j, const ModifySettingRequest<TSettings>& request)
{
    j = nlohmann::json{{"ResourcePath", request.ResourcePath}, {"RequestType", request.RequestType}};

    if constexpr (!std::is_same_v<TSettings, void>)
    {
        j["Settings"] = request.Settings;
    }
}

struct PropertyQuery
{
    std::vector<PropertyType> PropertyTypes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(PropertyQuery, PropertyTypes);
};

enum ProcessorFeature
{
    NestedVirt = 70
};

struct _Error // Renamed from 'Error' to make the macro happy.
{
    int32_t Error;
    std::string ErrorMessage;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(_Error, Error, ErrorMessage);
};

struct ProcessorCapabilitiesInfo
{
    std::vector<std::string> ProcessorFeatures;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ProcessorCapabilitiesInfo, ProcessorFeatures);
};

struct Version
{
    uint32_t Major;
    uint32_t Minor;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Version, Major, Minor);
};

struct BasicInformation
{
    std::vector<Version> SupportedSchemaVersions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BasicInformation, SupportedSchemaVersions);
};

template <typename TResponse>
struct PropertyResponse
{
    std::optional<_Error> Error;
    TResponse Response;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PropertyResponse<ProcessorCapabilitiesInfo>, Error, Response);
};

template <typename TResponse>
struct ServicePropertiesResponse
{
    std::map<std::string, TResponse> PropertyResponses;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ServicePropertiesResponse<PropertyResponse<ProcessorCapabilitiesInfo>>, PropertyResponses);
};

template <typename TResponse>
struct ServiceProperties
{
    std::vector<TResponse> Properties;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ServiceProperties<BasicInformation>, Properties);
};

enum class SystemType
{
    VirtualMachine = 1
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    SystemType,
    {
        {SystemType::VirtualMachine, "VirtualMachine"},
    })

struct Properties
{
    GUID RuntimeId;
    SystemType SystemType;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Properties, RuntimeId, SystemType);
};

enum class MemoryBackingPageSize
{
    Small = 0
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    MemoryBackingPageSize,
    {
        {MemoryBackingPageSize::Small, "Small"},
    })

struct Memory
{
    uint64_t SizeInMB;
    bool AllowOvercommit;
    bool EnableDeferredCommit;
    bool EnableColdDiscardHint;
    std::optional<MemoryBackingPageSize> BackingPageSize;
    std::optional<uint32_t> FaultClusterSizeShift;
    std::optional<uint32_t> DirectMapFaultClusterSizeShift;
    std::optional<uint64_t> HighMmioGapInMB;
    std::optional<uint64_t> HighMmioBaseInMB;
    std::optional<std::wstring> HostingProcessNameSuffix;
};

inline void to_json(nlohmann::json& j, const Memory& memory)
{
    j = nlohmann::json{
        {"SizeInMB", memory.SizeInMB},
        {"AllowOvercommit", memory.AllowOvercommit},
        {"EnableDeferredCommit", memory.EnableDeferredCommit},
        {"EnableColdDiscardHint", memory.EnableColdDiscardHint}};

    OMIT_IF_EMPTY(j, memory, BackingPageSize);
    OMIT_IF_EMPTY(j, memory, FaultClusterSizeShift);
    OMIT_IF_EMPTY(j, memory, DirectMapFaultClusterSizeShift);
    OMIT_IF_EMPTY(j, memory, HighMmioGapInMB);
    OMIT_IF_EMPTY(j, memory, HighMmioBaseInMB);
    OMIT_IF_EMPTY(j, memory, HostingProcessNameSuffix);
}

struct Processor
{
    uint32_t Count;
    std::optional<bool> ExposeVirtualizationExtensions;
    std::optional<bool> EnablePerfmonPmu;
    std::optional<bool> EnablePerfmonLbr;
};

inline void to_json(nlohmann::json& j, const Processor& processor)
{
    j = nlohmann::json{{"Count", processor.Count}};
    OMIT_IF_EMPTY(j, processor, ExposeVirtualizationExtensions)
    OMIT_IF_EMPTY(j, processor, EnablePerfmonPmu)
    OMIT_IF_EMPTY(j, processor, EnablePerfmonLbr)
}

struct Topology
{
    Processor Processor;
    Memory Memory;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Topology, Processor, Memory);
};

struct VirtioSerialPort
{
    std::wstring Name;
    std::wstring NamedPipe;
    bool ConsoleSupport;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(VirtioSerialPort, Name, NamedPipe, ConsoleSupport);
};

struct VirtioSerial
{
    std::map<std::string, VirtioSerialPort> Ports;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(VirtioSerial, Ports);
};

struct ComPort
{
    std::wstring NamedPipe;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ComPort, NamedPipe);
};

struct LinuxKernelDirect
{
    std::wstring KernelFilePath;
    std::wstring InitRdPath;
    std::wstring KernelCmdLine;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(LinuxKernelDirect, KernelFilePath, InitRdPath, KernelCmdLine);
};

enum class UefiBootDevice
{
    VmbFs
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    UefiBootDevice,
    {
        {UefiBootDevice::VmbFs, "VmbFs"},
    })

struct UefiBootEntry
{
    UefiBootDevice DeviceType;
    std::wstring VmbFsRootPath;
    std::wstring DevicePath;
    std::wstring OptionalData;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(UefiBootEntry, DeviceType, VmbFsRootPath, DevicePath, OptionalData);
};

struct Uefi
{
    UefiBootEntry BootThis;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Uefi, BootThis);
};

struct EmptyObject
{
};

inline void to_json(nlohmann::json& j, const EmptyObject& memory)
{
    j = nlohmann::json::object();
}

struct HvSocketSystemConfig
{
    std::wstring DefaultBindSecurityDescriptor;
    std::wstring DefaultConnectSecurityDescriptor;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(HvSocketSystemConfig, DefaultBindSecurityDescriptor, DefaultConnectSecurityDescriptor);
};

struct HvSocket
{
    HvSocketSystemConfig HvSocketConfig;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(HvSocket, HvSocketConfig);
};

struct Chipset
{
    bool UseUtc;
    std::optional<LinuxKernelDirect> LinuxKernelDirect;
    std::optional<Uefi> Uefi;
};

inline void to_json(nlohmann::json& j, const Chipset& chipset)
{
    j = nlohmann::json{{"UseUtc", chipset.UseUtc}};

    OMIT_IF_EMPTY(j, chipset, LinuxKernelDirect)
    OMIT_IF_EMPTY(j, chipset, Uefi)
}

struct Scsi
{
    std::map<std::string, EmptyObject> Attachments;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Scsi, Attachments);
};

struct DebugOptions
{
    std::optional<std::wstring> BugcheckSavedStateFileName;
    std::optional<std::wstring> ShutdownOrResetSavedStateFileName;
};

inline void to_json(nlohmann::json& j, const DebugOptions& d)
{
    j = nlohmann::json::object();
    OMIT_IF_EMPTY(j, d, BugcheckSavedStateFileName);
    OMIT_IF_EMPTY(j, d, ShutdownOrResetSavedStateFileName);
}

enum class VirtualPMemImageFormat
{
    Vhdx,
    Vhd1
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    VirtualPMemImageFormat,
    {
        {VirtualPMemImageFormat::Vhdx, "Vhdx"},
        {VirtualPMemImageFormat::Vhd1, "Vhd1"},
    })

struct VirtualPMemDevice
{
    std::wstring HostPath;
    bool ReadOnly;
    VirtualPMemImageFormat ImageFormat;
    // uint64_t SizeBytes;
    // std::map<uint64_t, VirtualPMemMapping> Mappings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(VirtualPMemDevice, HostPath, ReadOnly, ImageFormat);
};

enum class VirtualPMemBackingType
{
    Virtual,
    Physical
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    VirtualPMemBackingType,
    {
        {VirtualPMemBackingType::Virtual, "Virtual"},
        {VirtualPMemBackingType::Physical, "Physical"},
    })

struct VirtualPMemController
{
    std::map<std::string, VirtualPMemDevice> Devices;
    uint8_t MaximumCount;
    uint64_t MaximumSizeBytes;
    VirtualPMemBackingType Backing;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(VirtualPMemController, Devices, MaximumCount, MaximumSizeBytes, Backing);
};

struct Devices
{
    std::optional<VirtioSerial> VirtioSerial;
    std::map<std::string, ComPort> ComPorts;
    EmptyObject Plan9;
    EmptyObject Battery;
    HvSocket HvSocket;
    std::map<std::string, Scsi> Scsi;
    std::optional<VirtualPMemController> VirtualPMem;
};

inline void to_json(nlohmann::json& j, const Devices& devices)
{
    j = nlohmann::json{
        {"ComPorts", devices.ComPorts},
        {"Plan9", devices.Plan9},
        {"Battery", devices.Battery},
        {"HvSocket", devices.HvSocket},
        {"Scsi", devices.Scsi}};

    OMIT_IF_EMPTY(j, devices, VirtioSerial);
    OMIT_IF_EMPTY(j, devices, VirtualPMem);
}

struct VirtualMachine
{
    bool StopOnReset;
    Chipset Chipset;
    Topology ComputeTopology;
    Devices Devices;
    DebugOptions DebugOptions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(VirtualMachine, StopOnReset, Chipset, ComputeTopology, Devices, DebugOptions);
};

struct ComputeSystem
{
    std::wstring Owner;
    bool ShouldTerminateOnLastHandleClosed;
    Version SchemaVersion;
    VirtualMachine VirtualMachine;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ComputeSystem, Owner, ShouldTerminateOnLastHandleClosed, SchemaVersion, VirtualMachine)
};

struct GuestErrorSaveReport
{
    std::optional<std::wstring> SaveStateFile;
    std::optional<long> Status;
};

inline void to_json(nlohmann::json& j, const GuestErrorSaveReport& g)
{
    j = nlohmann::json::object();
    OMIT_IF_EMPTY(j, g, SaveStateFile);
    OMIT_IF_EMPTY(j, g, Status);
}

inline void from_json(const nlohmann::json& j, GuestErrorSaveReport& r)
{
    ASSIGN_IF_PRESENT(j, r, SaveStateFile);
    ASSIGN_IF_PRESENT(j, r, Status);
}

struct CrashReport
{
    std::wstring CrashLog;
    std::optional<GuestErrorSaveReport> GuestCrashSaveInfo;
};

inline void to_json(nlohmann::json& j, const CrashReport& c)
{
    j = nlohmann::json::object();
    j.at("CrashLog") = c.CrashLog;
    OMIT_IF_EMPTY(j, c, GuestCrashSaveInfo);
}

inline void from_json(const nlohmann::json& j, CrashReport& c)
{
    ASSIGN_IF_PRESENT(j, c, CrashLog);
    ASSIGN_IF_PRESENT(j, c, GuestCrashSaveInfo);
}

enum class NotificationType
{
    None,
    GracefulExit,
    ForcedExit,
    UnexpectedExit,
    Unknown
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    NotificationType,
    {
        {NotificationType::None, "None"},
        {NotificationType::GracefulExit, "GracefulExit"},
        {NotificationType::ForcedExit, "ForcedExit"},
        {NotificationType::UnexpectedExit, "UnexpectedExit"},
        {NotificationType::Unknown, "Unknown"},
    })

struct GuestCrashAttribution
{
    std::optional<std::vector<uint64_t>> CrashParameters;
};

inline void to_json(nlohmann::json& j, const GuestCrashAttribution& g)
{
    j = nlohmann::json::object();
    OMIT_IF_EMPTY(j, g, CrashParameters)
}

inline void from_json(const nlohmann::json& j, GuestCrashAttribution& g)
{
    ASSIGN_IF_PRESENT(j, g, CrashParameters);
}

// Attribution record (trimmed to GuestCrash only for now)
struct AttributionRecord
{
    std::optional<GuestCrashAttribution> GuestCrash;
};

inline void to_json(nlohmann::json& j, const AttributionRecord& a)
{
    j = nlohmann::json::object();
    OMIT_IF_EMPTY(j, a, GuestCrash)
}

inline void from_json(const nlohmann::json& j, AttributionRecord& a)
{
    ASSIGN_IF_PRESENT(j, a, GuestCrash);
}

struct SystemExitStatus
{
    int32_t Status;
    std::optional<NotificationType> ExitType;
    std::optional<std::vector<AttributionRecord>> Attribution;
};

inline void to_json(nlohmann::json& j, const SystemExitStatus& s)
{
    j = nlohmann::json{{"Status", s.Status}};
    OMIT_IF_EMPTY(j, s, ExitType);
    OMIT_IF_EMPTY(j, s, Attribution);
}

inline void from_json(const nlohmann::json& j, SystemExitStatus& s)
{
    s.Status = j.at("Status").get<int32_t>();
    ASSIGN_IF_PRESENT(j, s, ExitType);
    ASSIGN_IF_PRESENT(j, s, Attribution);
}

} // namespace wsl::windows::common::hcs

#undef OMIT_IF_EMPTY