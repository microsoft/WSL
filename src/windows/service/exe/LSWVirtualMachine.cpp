#include "LSWVirtualMachine.h"
#include "hcs_schema.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using helpers::WindowsVersion;
using wsl::windows::service::lsw::LSWVirtualMachine;

#define VIRTIO_SERIAL_CONSOLE_COBALT_RELEASE_UBR 40 // TODO: factor

LSWVirtualMachine::LSWVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID UserSid) :
    m_settings(Settings), m_userSid(UserSid)
{
    THROW_IF_FAILED(CoCreateGuid(&m_vmId));
}

void LSWVirtualMachine::Start()
{
    hcs::ComputeSystem systemSettings{};
    systemSettings.Owner = L"WSL";
    systemSettings.ShouldTerminateOnLastHandleClosed = true;
    systemSettings.SchemaVersion.Major = 2;
    systemSettings.SchemaVersion.Minor = 3;
    hcs::VirtualMachine vmSettings{};
    vmSettings.StopOnReset = true;
    vmSettings.Chipset.UseUtc = true;

    // Ensure the 2MB granularity enforced by HCS.
    vmSettings.ComputeTopology.Memory.SizeInMB = ((m_settings.MemoryMb / _1MB) & ~0x1);
    vmSettings.ComputeTopology.Memory.AllowOvercommit = true;
    vmSettings.ComputeTopology.Memory.EnableDeferredCommit = true;
    vmSettings.ComputeTopology.Memory.EnableColdDiscardHint = true;

    // Configure backing page size, fault cluster shift size, and cold discard hint size to favor density (lower vmmem usage).
    //
    // N.B. Cold discard hint size should be a multiple of the fault cluster shift size.
    //
    // N.B. This is only done on builds that have the fix for the VID deadlock on partition teardown.
    if ((m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Cobalt && m_windowsVersion.UpdateBuildRevision >= 2360) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Iron && m_windowsVersion.UpdateBuildRevision >= 1970) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Vibranium_22H2 && m_windowsVersion.UpdateBuildRevision >= 3393))
    {
        vmSettings.ComputeTopology.Memory.BackingPageSize = hcs::MemoryBackingPageSize::Small;
        vmSettings.ComputeTopology.Memory.FaultClusterSizeShift = 4;          // 64k
        vmSettings.ComputeTopology.Memory.DirectMapFaultClusterSizeShift = 4; // 64k
        m_coldDiscardShiftSize = 5;                                           // 128k
    }
    else
    {
        m_coldDiscardShiftSize = 9; // 2MB
    }

    // Configure the number of processors.
    vmSettings.ComputeTopology.Processor.Count = 4; // TODO

    // Set the vmmem suffix which will change the process name in task manager.
    // if (IsVmemmSuffixSupported()) // TODO: impl
    {
        vmSettings.ComputeTopology.Memory.HostingProcessNameSuffix = m_settings.DisplayName;
    }

    // TODO

    /*
    if (m_vmConfig.EnableHardwarePerformanceCounters)
    {
        HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
        __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
        vmSettings.ComputeTopology.Processor.EnablePerfmonPmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
        vmSettings.ComputeTopology.Processor.EnablePerfmonLbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;
    }*/

    // Initialize kernel command line.
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(LSW_ROOT_INIT_ENV) L"=1 panic=-1";

    // Set number of processors.
    kernelCmdLine += std::format(L" nr_cpus={}", m_settings.CpuCount);

    // Enable timesync workaround to sync on resume from sleep in modern standby.
    kernelCmdLine += L" hv_utils.timesync_implicit=1";

    // TODO: check for virtio serial support
    m_dmesgCollector = DmesgCollector::Create(m_vmId, m_vmExitEvent, true, false, L"", true);

    if (true) // early boot logging
    {
        kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
    }

    vmSettings.Devices.VirtioSerial.emplace();

    // TODO: support early boot logging

    // The primary "console" will be a virtio serial device.
    kernelCmdLine += L" console=hvc0 debug";
    hcs::VirtioSerialPort virtioPort{};
    virtioPort.Name = L"hvc0";
    virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
    virtioPort.ConsoleSupport = true;
    vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);

    // Set up boot params.
    //
    // N.B. Linux kernel direct boot is not yet supported on ARM64.

    auto basePath = wslutil::GetBasePath();
    auto kernelPath = LR"(D:\wsldev\kernel)";

    if constexpr (!wsl::shared::Arm64)
    {
        vmSettings.Chipset.LinuxKernelDirect.emplace();
        vmSettings.Chipset.LinuxKernelDirect->KernelFilePath = kernelPath;
        vmSettings.Chipset.LinuxKernelDirect->InitRdPath = (basePath / L"tools" / LXSS_VM_MODE_INITRD_NAME).c_str();
        vmSettings.Chipset.LinuxKernelDirect->KernelCmdLine = kernelCmdLine;
    }
    else
    {
        // TODO
        THROW_HR(E_NOTIMPL);
        auto bootThis = hcs::UefiBootEntry{};
        bootThis.DeviceType = hcs::UefiBootDevice::VmbFs;
        // bootThis.VmbFsRootPath = m_rootFsPath.c_str();
        bootThis.DevicePath = L"\\" LXSS_VM_MODE_KERNEL_NAME;
        bootThis.OptionalData = kernelCmdLine;
        hcs::Uefi uefiSettings{};
        uefiSettings.BootThis = std::move(bootThis);
        vmSettings.Chipset.Uefi = std::move(uefiSettings);
    }

    // Initialize other devices.
    vmSettings.Devices.Scsi["0"] = hcs::Scsi{};
    hcs::HvSocket hvSocketConfig{};

    // Construct a security descriptor that allows system and the current user.
    wil::unique_hlocal_string userSidString;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(m_userSid, &userSidString));

    std::wstring securityDescriptor{L"D:P(A;;FA;;;SY)(A;;FA;;;"};
    securityDescriptor += userSidString.get();
    securityDescriptor += L")";
    hvSocketConfig.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocketConfig.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    vmSettings.Devices.HvSocket = std::move(hvSocketConfig);

    systemSettings.VirtualMachine = std::move(vmSettings);
    auto json = wsl::shared::ToJsonW(systemSettings);

    WSL_LOG("CreateLSWVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_computeSystem = hcs::CreateComputeSystem(m_vmIdString.c_str(), json.c_str());

    auto runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_computeSystem.get());
    WI_ASSERT(IsEqualGUID(m_vmId, runtimeId));

    wsl::windows::common::hcs::RegisterCallback(m_computeSystem.get(), &s_OnExit, this);

    wsl::windows::common::hcs::StartComputeSystem(m_computeSystem.get(), json.c_str());

    // Create a socket listening for connections from mini_init.
    auto listenSocket = wsl::windows::common::hvsocket::Listen(runtimeId, LX_INIT_UTILITY_VM_INIT_PORT);
    auto socket = wsl::windows::common::hvsocket::Accept(listenSocket.get(), m_settings.BootTimeoutMs, m_vmTerminatingEvent.get());
    m_initChannel = wsl::shared::SocketChannel{std::move(socket), "mini_init", m_vmTerminatingEvent.get()};
}

void CALLBACK LSWVirtualMachine::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
{
    WSL_LOG("LSWVmExited", TraceLoggingValue(Event->EventData, "details"));
}

HRESULT LSWVirtualMachine::GetState()
{
    return S_OK;
}

HRESULT LSWVirtualMachine::AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly, _Out_ LPSTR* Device)
try
{
    *Device = nullptr;
    auto result = wil::ResultFromException([&]() {
        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        auto runAsUser = wil::impersonate_token(userToken.get());

        wsl::windows::common::hcs::GrantVmAccess(m_vmIdString.c_str(), Path);

        std::lock_guard lock{m_lock};

        ULONG lun = 0;
        while (m_attachedDisks.find(lun) != m_attachedDisks.end())
        {
            lun++;
        }

        wsl::windows::common::hcs::AddVhd(m_computeSystem.get(), Path, lun, ReadOnly);

        auto cleanup = wil::scope_exit_log(
            WI_DIAGNOSTICS_INFO, [&]() { wsl::windows::common::hcs::RemoveScsiDisk(m_computeSystem.get(), lun); });

        LSW_GET_DISK message{};
        message.Header.MessageSize = sizeof(message);
        message.Header.MessageType = LSW_GET_DISK::Type;
        message.ScsiLun = lun;
        const auto& response = m_initChannel.Transaction(message);

        THROW_HR_IF_MSG(E_FAIL, response.Result != 0, "Failed to attach disk, init returned: %lu", response.Result);

        cleanup.release();
        m_attachedDisks.emplace(lun, AttachedDisk{Path, response.Buffer});

        *Device = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(response.Buffer).release();
    });

    WSL_LOG(
        "LSWAttachDisk",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(*Device == nullptr ? "<null>" : *Device, "Device"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT LSWVirtualMachine::Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ BOOL Chroot)
try
{
    wsl::shared::MessageWriter<LSW_MOUNT> message;

    auto optionalAdd = [&](auto value, unsigned int& index) {
        if (Source != nullptr)
        {
            message.WriteString(index, value);
        }
    };

    optionalAdd(Source, message->SourceIndex);
    optionalAdd(Target, message->DestinationIndex);
    optionalAdd(Type, message->TypeIndex);
    optionalAdd(Options, message->OptionsIndex);
    message->Chroot = Chroot; // TODO: proper API

    std::lock_guard lock{m_lock};

    const auto& response = m_initChannel.Transaction<LSW_MOUNT>(message.Span());

    WSL_LOG(
        "LSWMount",
        TraceLoggingValue(Source == nullptr ? "<null>" : Source, "Source"),
        TraceLoggingValue(Target == nullptr ? "<null>" : Target, "Target"),
        TraceLoggingValue(Type == nullptr ? "<null>" : Type, "Type"),
        TraceLoggingValue(Options == nullptr ? "<null>" : Options, "Options"),
        TraceLoggingValue(Chroot, "Options"),
        TraceLoggingValue(response.Result, "Result"));

    // TODO: better error
    THROW_HR_IF(E_FAIL, response.Result != 0);
    return S_OK;
}
CATCH_RETURN();

HRESULT LSWVirtualMachine::CreateLinuxProcess(
    _In_ const LSW_CREATE_PROCESS_OPTIONS* Options, ULONG* FdCount, LSW_PROCESS_FD** Fds, _Out_ LSW_CREATE_PROCESS_RESULT* Result)
try
{
    wsl::shared::MessageWriter<LSW_CREATE_PROCESS> Message;

    Message.WriteString(Message->ExecutableIndex, Options->Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options->CurrentDirectory ? Options->CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options->CommandLine, Options->CommandLineCount);
    Message.WriteStringArray(Message->EnvironmentIndex, Options->Environmnent, Options->EnvironmnentCount);

    std::lock_guard lock{m_lock};
    const auto& port = m_initChannel.Transaction<LSW_CREATE_PROCESS>(Message.Span());

    std::vector<wil::unique_socket> sockets(3);

    for (auto& e : sockets)
    {
        e = wsl::windows::common::hvsocket::Connect(m_vmId, port.Result);
    }

    const auto& response = m_initChannel.ReceiveMessage<LSW_CREATE_PROCESS_RESPONSE>();

    Result->Errno = response.Result;
    Result->Pid = response.Pid;

    *Fds = wil::make_unique_cotaskmem<LSW_PROCESS_FD[]>(sockets.size()).release();
    *FdCount = static_cast<ULONG>(sockets.size());

    for (size_t i = 0; i < sockets.size(); i++)
    {
        (*Fds)[i].Handle = (HANDLE)sockets[i].release();
    }

    return S_OK;
}
CATCH_RETURN();