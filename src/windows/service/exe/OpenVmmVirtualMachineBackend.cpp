// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmVirtualMachineBackend.h"
#include <afunix.h>
#include <bitset>
#include "SubProcess.h"
#include "wslopenvmm.h"

namespace {

constexpr UINT64 c_memoryGranularity = 2ULL * 1024 * 1024;
constexpr UINT64 c_maximumMemory = 4ULL * 1024 * 1024 * 1024;
constexpr UINT32 c_maximumDisks = 254;
constexpr UINT32 c_rpcTimeoutMs = 30000;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

void DestroyConfig(WslOpenVmmConfig* Config) noexcept
{
    WslOpenVmmDestroyConfig(&Config);
}

using UniqueConfig = wil::unique_any<WslOpenVmmConfig*, decltype(&DestroyConfig), DestroyConfig>;

void ValidateFeature(VmFeatureRequest Request, PCWSTR Setting)
{
    switch (Request)
    {
    case VmFeatureRequest::Disabled:
    case VmFeatureRequest::Preferred:
        return;
    case VmFeatureRequest::Required:
        THROW_HR_MSG(c_notSupported, "OpenVMM does not support the required %ls setting", Setting);
    }

    THROW_HR(E_INVALIDARG);
}

void ValidatePath(const std::filesystem::path& Path)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        Path.empty() || !Path.is_absolute() || Path.native().find(L'\0') != std::wstring::npos,
        "OpenVMM requires an absolute, nonempty host path");
}

void ValidateConsolePath(const std::filesystem::path& Path)
{
    ValidatePath(Path);
    THROW_HR_IF_MSG(
        c_notSupported, !Path.native().starts_with(L"\\\\.\\pipe\\"), "OpenVMM consoles require a caller-provided named pipe");
}

void DeleteOwnedFile(const std::filesystem::path& Path) noexcept
{
    if (!Path.empty() && !DeleteFileW(Path.c_str()))
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            LOG_WIN32(error);
        }
    }
}

} // namespace

VmDescription wsl::windows::common::vm::openvmm::ValidateCreateRequest(const VmCreateRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.VmId, GUID_NULL));
    THROW_HR_IF(E_INVALIDARG, Request.Processor.Count == 0 || Request.Memory.SizeBytes == 0);
    THROW_HR_IF_MSG(c_notSupported, wsl::shared::Arm64, "OpenVMM direct boot is currently supported only on x64");
    ValidatePath(Request.Boot.KernelPath);
    ValidatePath(Request.Boot.InitrdPath);
    THROW_HR_IF(
        E_INVALIDARG,
        Request.Boot.GuestCommandLine.find(L'\0') != std::wstring::npos || Request.Boot.UserCommandLine.find(L'\0') != std::wstring::npos);
    THROW_HR_IF(
        E_INVALIDARG,
        Request.Boot.Method != VmBootMethod::Automatic && Request.Boot.Method != VmBootMethod::LinuxDirect &&
            Request.Boot.Method != VmBootMethod::LinuxFirmware);
    THROW_HR_IF(c_notSupported, Request.Boot.Method == VmBootMethod::LinuxFirmware);
    THROW_HR_IF(c_notSupported, Request.Boot.RequestedDmaBounceBufferBytes.has_value());

    VmDescription description;
    description.Identity.VmId = Request.VmId;
    description.Backend = BackendKind::OpenVmm;
    description.OwnerName = Request.OwnerName;
    description.HostingProcessName = Request.HostingProcessName;
    description.Processor.Count = Request.Processor.Count;
    description.Memory.SizeBytes = Request.Memory.SizeBytes;
    THROW_HR_IF(E_INVALIDARG, Request.Memory.SizeBytes % c_memoryGranularity != 0);

    THROW_HR_IF_MSG(
        c_notSupported,
        description.Memory.SizeBytes < c_memoryGranularity || Request.Memory.SizeBytes > c_maximumMemory,
        "OpenVMM currently supports memory sizes from 2 MiB to 4 GiB; memory is not silently capped");
    ValidateFeature(Request.Processor.NestedVirtualization, L"nested virtualization");
    ValidateFeature(Request.Processor.PerfmonPmu, L"PMU");
    ValidateFeature(Request.Processor.PerfmonLbr, L"LBR");
    ValidateFeature(Request.Memory.AllowOvercommit, L"memory overcommit");
    ValidateFeature(Request.Memory.DeferredCommit, L"deferred memory commit");
    ValidateFeature(Request.Memory.ColdDiscard, L"cold discard");

    if (Request.CrashCapture)
    {
        THROW_HR_IF(E_INVALIDARG, Request.CrashCapture->Policy != VmSelectionPolicy::Required && Request.CrashCapture->Policy != VmSelectionPolicy::Preferred);
        THROW_HR_IF(c_notSupported, Request.CrashCapture->Policy == VmSelectionPolicy::Required);
    }

    description.Boot.Method = VmBootMethod::LinuxDirect;
    description.Boot.KernelCommandLine = Request.Boot.GuestCommandLine;
    if (!Request.Boot.UserCommandLine.empty())
    {
        if (!description.Boot.KernelCommandLine.empty())
        {
            description.Boot.KernelCommandLine += L" ";
        }
        description.Boot.KernelCommandLine += Request.Boot.UserCommandLine;
    }

    bool serialConfigured = false;
    bool virtioConfigured = false;
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            THROW_HR_IF(c_notSupported, serial->Port != 0);
            THROW_HR_IF(E_INVALIDARG, serialConfigured);
            ValidateConsolePath(serial->NamedPipe);
            serialConfigured = true;
        }
        else
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, virtio.Port != 0 || !virtio.GuestName.empty());
            THROW_HR_IF(E_INVALIDARG, virtioConfigured);
            ValidateConsolePath(virtio.NamedPipe);
            virtioConfigured = true;
        }
        description.Boot.Consoles.push_back(console);
    }

    THROW_HR_IF(c_notSupported, Request.BootDisks.size() > c_maximumDisks);
    std::bitset<c_maximumDisks> allocated;
    for (const auto& disk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, disk.Key.empty() || description.BootDisks.contains(disk.Key));
        description.BootDisks.emplace(disk.Key, VmDiskAttachment{});
        const auto* source = std::get_if<VmVirtualDiskSource>(&disk.Disk.Source);
        THROW_HR_IF(c_notSupported, source == nullptr);
        ValidatePath(source->Path);
        switch (source->Format)
        {
        case VmDiskFormat::Vhd:
            THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhd") != 0);
            break;
        case VmDiskFormat::Vhdx:
            THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhdx") != 0);
            break;
        default:
            THROW_HR(E_INVALIDARG);
        }
        if (disk.Disk.Placement)
        {
            const auto& placement = *disk.Disk.Placement;
            THROW_HR_IF(c_notSupported, placement.Address.Controller != 0 || placement.Address.Lun >= c_maximumDisks);
            THROW_HR_IF(E_INVALIDARG, placement.Policy != VmPlacementPolicy::Exact && placement.Policy != VmPlacementPolicy::Preferred);
            if (placement.Policy == VmPlacementPolicy::Exact)
            {
                THROW_HR_IF(E_INVALIDARG, allocated.test(placement.Address.Lun));
                allocated.set(placement.Address.Lun);
            }
        }
    }

    std::uint64_t nextId = 1;
    for (const auto& disk : Request.BootDisks)
    {
        std::uint32_t lun = 0;
        if (disk.Disk.Placement && disk.Disk.Placement->Policy == VmPlacementPolicy::Exact)
        {
            lun = disk.Disk.Placement->Address.Lun;
        }
        else
        {
            if (disk.Disk.Placement && !allocated.test(disk.Disk.Placement->Address.Lun))
            {
                lun = disk.Disk.Placement->Address.Lun;
            }
            else
            {
                while (lun < c_maximumDisks && allocated.test(lun))
                {
                    ++lun;
                }
            }
            THROW_HR_IF(E_BOUNDS, lun == c_maximumDisks);
            allocated.set(lun);
        }
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
    }

    return description;
}

void OpenVmmVirtualMachineBackend::DestroyVm(WslOpenVmmVm* Vm) noexcept
{
    WslOpenVmmDestroyVm(&Vm);
}

OpenVmmVirtualMachineBackend::OpenVmmVirtualMachineBackend() : m_state(std::make_unique<State>())
{
}

OpenVmmVirtualMachineBackend::~OpenVmmVirtualMachineBackend() noexcept
{
    if (m_state->m_processWait)
    {
        SetThreadpoolWait(m_state->m_processWait.get(), nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(m_state->m_processWait.get(), TRUE);
        m_state->m_processWait.reset();
    }
    if (m_state->m_vm && WaitForSingleObject(m_state->m_process.get(), 0) == WAIT_TIMEOUT)
    {
        LOG_IF_FAILED(WslOpenVmmVmTeardown(m_state->m_vm.get()));
        LOG_IF_FAILED(WslOpenVmmVmQuit(m_state->m_vm.get()));
    }
    m_state->m_vm.reset();
    m_state->m_job.reset();
    if (m_state->m_process)
    {
        // Confirm exit before releasing backing files or deleting socket paths.
        LOG_LAST_ERROR_IF(WaitForSingleObject(m_state->m_process.get(), INFINITE) == WAIT_FAILED);
    }
    m_state->m_backingFiles.clear();
    if (m_state->m_directoryCreated)
    {
        DeleteOwnedFile(m_state->m_rpcSocketPath);
        DeleteOwnedFile(m_state->m_vsockPath);
        DeleteOwnedFile(m_state->m_socketDirectory / L"openvmm.log");
        LOG_IF_WIN32_BOOL_FALSE(RemoveDirectoryW(m_state->m_socketDirectory.c_str()));
    }
}

std::unique_ptr<OpenVmmVirtualMachineBackend> OpenVmmVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    auto description = wsl::windows::common::vm::openvmm::ValidateCreateRequest(Request);
    auto backend = std::unique_ptr<OpenVmmVirtualMachineBackend>{new OpenVmmVirtualMachineBackend{}};
    backend->m_state->m_description = std::move(description);
    backend->Initialize(Request);
    return backend;
}

void OpenVmmVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    using namespace wsl::windows::common;
    const auto executable = wslutil::GetBasePath() / L"openvmm.exe";
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !filesystem::FileExists(executable.c_str()),
        "openvmm.exe not found at: %ls",
        executable.c_str());

    auto id = wsl::shared::string::GuidToString<wchar_t>(Request.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::erase(id, L'-');
    // An exclusive directory creation prevents shortened path IDs from aliasing another VM.
    m_state->m_socketDirectory = filesystem::GetTempFolderPath(GetCurrentProcessToken()) / (L"ov-" + id.substr(0, 16));
    m_state->m_rpcSocketPath = m_state->m_socketDirectory / L"r";
    m_state->m_vsockPath = m_state->m_socketDirectory / L"v";
    const auto longestPath =
        wsl::shared::string::WideToMultiByte(m_state->m_vsockPath.native() + L"_ffffffff-facb-11e6-bd58-64006a7986d3");
    SOCKADDR_UN address{};
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        longestPath.size() >= sizeof(address.sun_path),
        "OpenVMM guest socket path exceeds the AF_UNIX limit: %hs",
        longestPath.c_str());
    THROW_HR_IF(E_INVALIDARG, m_state->m_rpcSocketPath.native().find_first_of(L",\"\r\n") != std::wstring::npos);

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
    const auto sid = wslutil::SidToString(tokenUser->User.Sid);
    const auto sddl = std::format(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;{})", sid.get());
    wil::unique_hlocal_security_descriptor security;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
    {
        auto openFile = [&](const std::filesystem::path& path, bool readOnly) {
            wil::unique_hfile file{CreateFileW(
                path.c_str(), GENERIC_READ | (readOnly ? 0 : GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            THROW_LAST_ERROR_IF(!file);
            file.reset();
            // Pin the file without conflicting with the VMM's disk sharing mode.
            file.reset(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            m_state->m_backingFiles.push_back(std::move(file));
        };
        openFile(Request.Boot.KernelPath, true);
        openFile(Request.Boot.InitrdPath, true);
        for (const auto& disk : Request.BootDisks)
        {
            openFile(std::get<VmVirtualDiskSource>(disk.Disk.Source).Path, disk.Disk.ReadOnly);
        }
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(m_state->m_socketDirectory.c_str(), &attributes));
        m_state->m_directoryCreated = true;
    }

    UniqueConfig config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(config.get(), Request.Boot.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(config.get(), Request.Boot.InitrdPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(config.get(), m_state->m_description.Boot.KernelCommandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(config.get(), m_state->m_description.Memory.SizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(config.get(), Request.Processor.Count));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(config.get(), m_state->m_vsockPath.c_str()));
    for (const auto& disk : Request.BootDisks)
    {
        const auto& attachment = m_state->m_description.BootDisks.at(disk.Key);
        THROW_IF_FAILED(WslOpenVmmConfigAddBootDisk(
            config.get(),
            attachment.GuestAddress.Controller,
            attachment.GuestAddress.Lun,
            std::get<VmVirtualDiskSource>(disk.Disk.Source).Path.c_str(),
            disk.Disk.ReadOnly));
    }
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            THROW_IF_FAILED(WslOpenVmmConfigAddSerialPort(config.get(), serial->Port, serial->NamedPipe.c_str()));
        }
        else
        {
            THROW_IF_FAILED(WslOpenVmmConfigSetVirtioConsolePath(config.get(), std::get<VmVirtioConsole>(console.Device).NamedPipe.c_str()));
        }
    }

    m_state->m_job = helpers::CreateKillOnCloseJob();
    const auto commandLine =
        std::format(L"\"{}\" --rpc \"path={},transport=grpc\"", executable.native(), m_state->m_rpcSocketPath.native());
    SubProcess process{executable.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
    process.SetJobObject(m_state->m_job.get());
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    wil::unique_hfile logFile;
    wil::unique_hfile input;
    {
        logFile.reset(CreateFileW(
            (m_state->m_socketDirectory / L"openvmm.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!logFile);
        input.reset(CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!input);
    }
    // OpenVMM closes stdout at startup; stderr must have a distinct handle value.
    wil::unique_hfile errorLogFile;
    THROW_IF_WIN32_BOOL_FALSE(
        DuplicateHandle(GetCurrentProcess(), logFile.get(), GetCurrentProcess(), errorLogFile.put(), 0, TRUE, DUPLICATE_SAME_ACCESS));
    process.SetStdHandles(input.get(), logFile.get(), errorLogFile.get());
    m_state->m_process = process.Start();
    m_state->m_processWait.reset(CreateThreadpoolWait(OnProcessExit, this, nullptr));
    THROW_LAST_ERROR_IF(!m_state->m_processWait);
    SetThreadpoolWait(m_state->m_processWait.get(), m_state->m_process.get(), nullptr);
    THROW_IF_FAILED_MSG(
        WslOpenVmmCreateVm(config.addressof(), m_state->m_rpcSocketPath.c_str(), c_rpcTimeoutMs, m_state->m_vm.put()),
        "Failed to create OpenVMM VM");
    const auto result = WaitForSingleObject(m_state->m_process.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED), result == WAIT_OBJECT_0);
}

void CALLBACK OpenVmmVirtualMachineBackend::OnProcessExit(PTP_CALLBACK_INSTANCE, void* Context, PTP_WAIT, TP_WAIT_RESULT) noexcept
{
    auto& backend = *static_cast<OpenVmmVirtualMachineBackend*>(Context);
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(backend.m_state->m_exitEvent.get()));
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities;
    capabilities.Backend = BackendKind::OpenVmm;
    if constexpr (!wsl::shared::Arm64)
    {
        capabilities.Operations[VmOperation::Create] = {true, true, false, false, false};
        for (const auto feature : {VmFeature::LinuxDirectBoot, VmFeature::Vhd, VmFeature::Vhdx, VmFeature::SerialConsole, VmFeature::VirtioConsole})
        {
            capabilities.Features[feature] = {true, true, false, false, false, {}};
        }
        capabilities.Features[VmFeature::SerialConsole].Limitation = L"Only port 0 with a caller-provided named pipe.";
        capabilities.Features[VmFeature::VirtioConsole].Limitation =
            L"Only port 0, with no guest name and a caller-provided named pipe.";
    }
    return capabilities;
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::GetCapabilities() const
{
    return QueryCapabilities();
}

wil::unique_handle OpenVmmVirtualMachineBackend::GetTerminationEvent() const
{
    wil::unique_handle event;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), m_state->m_exitEvent.get(), GetCurrentProcess(), event.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    return event;
}

void OpenVmmVirtualMachineBackend::Start()
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::Terminate()
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::CancelPendingOperations() noexcept
{
    LOG_HR(E_NOTIMPL);
}

VmGuestListener OpenVmmVirtualMachineBackend::CreateGuestListener(GuestServicePort)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket OpenVmmVirtualMachineBackend::AcceptGuestConnection(VmListenerId)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket OpenVmmVirtualMachineBackend::ConnectGuest(GuestServicePort)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::CloseGuestListener(VmListenerId)
{
    THROW_HR(E_NOTIMPL);
}

VmDiskAttachment OpenVmmVirtualMachineBackend::AttachDisk(const VmDiskRequest&)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::DetachDisk(VmDiskId)
{
    THROW_HR(E_NOTIMPL);
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest&)
{
    THROW_HR(E_NOTIMPL);
}

VmFileSystemShare OpenVmmVirtualMachineBackend::AddFileSystemShare(VmDeviceId, const VmFileSystemShareRequest&)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::RemoveFileSystemShare(VmShareId)
{
    THROW_HR(E_NOTIMPL);
}

VmNetworkAttachment OpenVmmVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest&)
{
    THROW_HR(E_NOTIMPL);
}

VmPortBinding OpenVmmVirtualMachineBackend::BindPort(VmDeviceId, const VmPortBindingRequest&)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::UnbindPort(VmPortBindingId)
{
    THROW_HR(E_NOTIMPL);
}
