/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWVirtualMachine.cpp

Abstract:

    TODO

--*/
#include "LSWVirtualMachine.h"
#include "hcs_schema.h"
#include "LSWApi.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using helpers::WindowsVersion;
using wsl::windows::service::lsw::LSWVirtualMachine;

#define VIRTIO_SERIAL_CONSOLE_COBALT_RELEASE_UBR 40 // TODO: factor

LSWVirtualMachine::LSWVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID UserSid) :
    m_settings(Settings), m_userSid(UserSid)
{
    THROW_IF_FAILED(CoCreateGuid(&m_vmId));

    if (Settings.EnableDebugShell)
    {
        m_debugShellPipe = wsl::windows::common::wslutil::GetDebugShellPipeName(m_userSid) + m_settings.DisplayName;
    }
}

HRESULT LSWVirtualMachine::GetDebugShellPipe(LPWSTR* pipePath)
{
    RETURN_HR_IF(E_INVALIDARG, m_debugShellPipe.empty());

    *pipePath = wil::make_unique_string<wil::unique_cotaskmem_string>(m_debugShellPipe.c_str()).release();

    return S_OK;
}

LSWVirtualMachine::~LSWVirtualMachine()
{
    WSL_LOG("LswTerminateVmStart", TraceLoggingValue(m_running, "running"));
    m_vmTerminatingEvent.SetEvent();

    m_initChannel.Close();

    bool forceTerminate = false;
    // Wait up to 5 seconds for the VM to terminate.
    if (!m_vmExitEvent.wait(5000))
    {
        forceTerminate = true;
        try
        {
            wsl::windows::common::hcs::TerminateComputeSystem(m_computeSystem.get());
        }
        CATCH_LOG()
    }

    WSL_LOG("LswTerminateVm", TraceLoggingValue(forceTerminate, "forced"), TraceLoggingValue(m_running, "running"));

    m_computeSystem.reset();

    for (const auto& e : m_attachedDisks)
    {
        try
        {
            wsl::windows::common::hcs::RevokeVmAccess(m_vmIdString.c_str(), e.second.Path.c_str());
        }
        CATCH_LOG()
    }
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

    wil::unique_handle dmesgOutput;

    if (m_settings.DmesgOutput != 0)
    {
        dmesgOutput.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(m_settings.DmesgOutput)));
    }

    m_dmesgCollector = DmesgCollector::Create(m_vmId, m_vmExitEvent, true, false, L"", true, std::move(dmesgOutput));

    if (false) // early boot logging
    {
        kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
    }

    vmSettings.Devices.VirtioSerial.emplace();

    // TODO: support early boot logging

    // The primary "console" will be a virtio serial device.

    if (true)
    {
        kernelCmdLine += L" console=hvc0 debug";
        hcs::VirtioSerialPort virtioPort{};
        virtioPort.Name = L"hvc0";
        virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);
    }

    if (!m_debugShellPipe.empty())
    {
        hcs::VirtioSerialPort virtioPort;
        virtioPort.Name = L"hvc1";
        virtioPort.NamedPipe = m_debugShellPipe;
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["1"] = std::move(virtioPort);
    }

    // Set up boot params.
    //
    // N.B. Linux kernel direct boot is not yet supported on ARM64.

    auto basePath = wslutil::GetBasePath();

#ifdef WSL_KERNEL_PATH

    auto kernelPath = std::filesystem::path(WSL_KERNEL_PATH);

#else
    auto kernelPath = std::filesystem::path(basePath) / L"tools" / LXSS_VM_MODE_KERNEL_NAME;
#endif

    if constexpr (!wsl::shared::Arm64)
    {
        vmSettings.Chipset.LinuxKernelDirect.emplace();
        vmSettings.Chipset.LinuxKernelDirect->KernelFilePath = kernelPath.wstring();
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

void LSWVirtualMachine::ConfigureNetworking()
{
    if (m_settings.NetworkingMode == NetworkingModeNone)
    {
        return;
    }

    if (m_settings.NetworkingMode == NetworkingModeNAT)
    {
        // TODO
    }
}

void CALLBACK LSWVirtualMachine::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
{
    if (Event->Type == HcsEventSystemExited || Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        reinterpret_cast<LSWVirtualMachine*>(Context)->OnExit(Event);
    }
}

void LSWVirtualMachine::OnExit(_In_ const HCS_EVENT* Event)
{
    WSL_LOG(
        "LSWVmExited", TraceLoggingValue(Event->EventData, "details"), TraceLoggingValue(static_cast<int>(Event->Type), "type"));

    m_vmExitEvent.SetEvent();

    std::lock_guard lock(m_lock);
    if (m_terminationCallback)
    {
        // TODO: parse json and give a better error.
        VirtualMachineTerminationReason reason = VirtualMachineTerminationReasonUnknown;
        if (Event->Type == HcsEventSystemExited)
        {
            reason = VirtualMachineTerminationReasonShutdown;
        }
        else if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
        {
            reason = VirtualMachineTerminationReasonCrashed;
        }

        LOG_IF_FAILED(m_terminationCallback->OnTermination(static_cast<ULONG>(reason), Event->EventData));
    }
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
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

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

HRESULT LSWVirtualMachine::Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags)
try
{
    static_assert(MountFlagsNone == LSW_MOUNT::None);
    static_assert(MountFlagsChroot == LSW_MOUNT::Chroot);
    static_assert(MountFlagsWriteableOverlayFs == LSW_MOUNT::OverlayFs);

    wsl::shared::MessageWriter<LSW_MOUNT> message;

    auto optionalAdd = [&](auto value, unsigned int& index) {
        if (value != nullptr)
        {
            message.WriteString(index, value);
        }
    };

    optionalAdd(Source, message->SourceIndex);
    optionalAdd(Target, message->DestinationIndex);
    optionalAdd(Type, message->TypeIndex);
    optionalAdd(Options, message->OptionsIndex);
    message->Flags = Flags;

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    const auto& response = m_initChannel.Transaction<LSW_MOUNT>(message.Span());

    WSL_LOG(
        "LSWMount",
        TraceLoggingValue(Source == nullptr ? "<null>" : Source, "Source"),
        TraceLoggingValue(Target == nullptr ? "<null>" : Target, "Target"),
        TraceLoggingValue(Type == nullptr ? "<null>" : Type, "Type"),
        TraceLoggingValue(Options == nullptr ? "<null>" : Options, "Options"),
        TraceLoggingValue(Flags, "Flags"),
        TraceLoggingValue(response.Result, "Result"));

    // TODO: better error
    THROW_HR_IF(E_FAIL, response.Result != 0);
    return S_OK;
}
CATCH_RETURN();

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> LSWVirtualMachine::Fork(enum LSW_FORK::ForkType Type)
{
    std::lock_guard lock{m_lock};
    return Fork(m_initChannel, Type);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> LSWVirtualMachine::Fork(wsl::shared::SocketChannel& Channel, enum LSW_FORK::ForkType Type)
{
    uint32_t port{};
    int32_t pid{};
    int32_t ptyMaster{};
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

        LSW_FORK message;
        message.ForkType = Type;
        message.TtyColumns = 80;
        message.TtyRows = 80;
        const auto& response = Channel.Transaction(message);
        port = response.Port;
        pid = response.Pid;
        ptyMaster = response.PtyMasterFd;
    }

    THROW_HR_IF_MSG(E_FAIL, pid <= 0, "fork() returned %i", pid);

    auto socket = wsl::windows::common::hvsocket::Connect(m_vmId, port, m_vmExitEvent.get(), m_settings.BootTimeoutMs);

    // TODO: pid in channel name
    return std::make_tuple(pid, ptyMaster, wsl::shared::SocketChannel{std::move(socket), "ForkedChannel"});
}

wil::unique_socket LSWVirtualMachine::ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd)
{
    LSW_CONNECT message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LSW_CONNECT::Type;
    message.Fd = Fd;
    const auto& response = Channel.Transaction(message);

    return wsl::windows::common::hvsocket::Connect(m_vmId, response.Result);
}

HRESULT LSWVirtualMachine::CreateLinuxProcess(
    _In_ const LSW_CREATE_PROCESS_OPTIONS* Options, ULONG FdCount, LSW_PROCESS_FD* Fds, HANDLE* Handles, _Out_ LSW_CREATE_PROCESS_RESULT* Result)
try
{
    // Check if this is a tty or not
    const LSW_PROCESS_FD* ttyInput = nullptr;
    const LSW_PROCESS_FD* ttyOutput = nullptr;
    auto interactiveTty = ParseTtyInformation(Fds, FdCount, &ttyInput, &ttyOutput);
    auto [pid, _, childChannel] = Fork(LSW_FORK::Process);

    std::vector<wil::unique_socket> sockets(FdCount);
    for (size_t i = 0; i < FdCount; i++)
    {
        sockets[i] = ConnectSocket(childChannel, static_cast<int32_t>(Fds[i].Fd));
    }

    wsl::shared::MessageWriter<LSW_EXEC> Message;

    Message.WriteString(Message->ExecutableIndex, Options->Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options->CurrentDirectory ? Options->CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options->CommandLine, Options->CommandLineCount);
    Message.WriteStringArray(Message->EnvironmentIndex, Options->Environmnent, Options->EnvironmnentCount);

    // If this is an interactive tty, we need a relay process
    if (interactiveTty)
    {
        auto [grandChildPid, ptyMaster, grandChildChannel] = Fork(childChannel, LSW_FORK::Pty);
        LSW_TTY_RELAY relayMessage;
        relayMessage.TtyMaster = ptyMaster;
        relayMessage.TtyInput = ttyInput->Fd;
        relayMessage.TtyOutput = ttyOutput->Fd;
        childChannel.SendMessage(relayMessage);

        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            Result->Errno = result;
            return E_FAIL;
        }

        grandChildChannel.SendMessage<LSW_EXEC>(Message.Span());
        result = ExpectClosedChannelOrError(grandChildChannel);
        if (result != 0)
        {
            Result->Errno = result;
            return E_FAIL;
        }

        pid = grandChildPid;
    }
    else
    {
        childChannel.SendMessage<LSW_EXEC>(Message.Span());
        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            Result->Errno = result;
            return E_FAIL;
        }
    }

    Result->Errno = 0;
    Result->Pid = pid;

    for (size_t i = 0; i < sockets.size(); i++)
    {
        Handles[i] = (HANDLE)sockets[i].release();
    }

    return S_OK;
}
CATCH_RETURN();

int32_t LSWVirtualMachine::ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel)
{
    auto [response, span] = Channel.ReceiveMessageOrClosed<RESULT_MESSAGE<int32_t>>();
    if (response != nullptr)
    {
        return response->Result;
    }
    else
    {
        return 0;
    }
}

HRESULT LSWVirtualMachine::WaitPid(LONG Pid, ULONGLONG TimeoutMs, ULONG* State, int* Code)
try
{
    auto [pid, _, subChannel] = Fork(LSW_FORK::Thread);

    LSW_WAITPID message{};
    message.Pid = Pid;
    message.TimeoutMs = TimeoutMs;

    const auto& response = subChannel.Transaction(message);

    THROW_HR_IF(E_FAIL, response.State == LSWProcessStateUnknown);

    *State = response.State;
    *Code = response.Code;

    return S_OK;
}
CATCH_RETURN();

HRESULT LSWVirtualMachine::Shutdown(ULONGLONG TimeoutMs)
try
{
    std::lock_guard lock(m_lock);

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    LSW_SHUTDOWN message{};
    m_initChannel.SendMessage(message);
    auto response = m_initChannel.ReceiveMessageOrClosed<MESSAGE_HEADER>(static_cast<wsl::shared::TTimeout>(TimeoutMs));

    RETURN_HR_IF(E_UNEXPECTED, response.first != nullptr);

    m_running = false;
    return S_OK;
}
CATCH_RETURN();

HRESULT LSWVirtualMachine::Signal(_In_ LONG Pid, _In_ int Signal)
try
{
    std::lock_guard lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    LSW_SIGNAL message;
    message.Pid = Pid;
    message.Signal = Signal;
    const auto& response = m_initChannel.Transaction(message);

    RETURN_HR_IF(E_FAIL, response.Result != 0);
    return S_OK;
}
CATCH_RETURN();

HRESULT LSWVirtualMachine::RegisterCallback(ITerminationCallback* callback)
try
{
    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_INVALIDARG, m_terminationCallback);

    // N.B. this calls AddRef() on the callback
    m_terminationCallback = callback;

    return S_OK;
}
CATCH_RETURN();

bool LSWVirtualMachine::ParseTtyInformation(const LSW_PROCESS_FD* Fds, ULONG FdCount, const LSW_PROCESS_FD** TtyInput, const LSW_PROCESS_FD** TtyOutput)
{
    bool foundNonTtyFd = false;

    for (ULONG i = 0; i < FdCount; i++)
    {
        if (Fds[i].Type == TerminalInput)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, *TtyInput != nullptr, "Only one TtyInput fd can be passed. Index=%lu", i);

            *TtyInput = &Fds[i];
        }
        else if (Fds[i].Type == TerminalOutput)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, *TtyOutput != nullptr, "Only one TtyOutput fd can be passed. Index=%lu", i);
            *TtyOutput = &Fds[i];
        }
        else
        {
            foundNonTtyFd = true;
        }
    }

    THROW_HR_IF_MSG(
        E_INVALIDARG, foundNonTtyFd && (*TtyOutput != nullptr || *TtyInput != nullptr), "Found mixed tty & non tty fds");

    return !foundNonTtyFd && FdCount > 0;
}
