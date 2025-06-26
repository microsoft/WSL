#include "LSWVirtualMachine.h"
#include "hcs_schema.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using helpers::WindowsVersion;
using wsl::windows::service::lsw::LSWVirtualMachine;

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
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSL_ROOT_INIT_ENV) L"=1 panic=-1";

    // Set number of processors.
    kernelCmdLine += std::format(L" nr_cpus={}", m_settings.CpuCount);

    // Enable timesync workaround to sync on resume from sleep in modern standby.
    kernelCmdLine += L" hv_utils.timesync_implicit=1";

    /*if (IsVirtioSerialConsoleSupported())
    {
        vmSettings.Devices.VirtioSerial.emplace();
    }

    if (m_dmesgCollector)
    {
        if (m_vmConfig.EnableEarlyBootLogging)
        {
            // Capture using the very slow legacy serial port up until the point that the virtio device is started.
            if constexpr (!wsl::shared::Arm64)
            {
                kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
            }
            else
            {
                kernelCmdLine += L" earlycon=pl011,0xeffec000,115200";
            }

            vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
        }

        // The primary "console" will be a virtio serial device.
        kernelCmdLine += L" console=hvc0 debug";
        hcs::VirtioSerialPort virtioPort{};
        virtioPort.Name = L"hvc0";
        virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);
    }
    else if (m_vmConfig.EnableDebugConsole)
    {
        // If a debug console was requested, add required kernel command line options.
        if constexpr (!wsl::shared::Arm64)
        {
            kernelCmdLine += L" console=ttyS0,115200 debug";
        }
        else
        {
            kernelCmdLine += L" console=ttyAMA0 debug";
        }
    }*/

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
        //bootThis.VmbFsRootPath = m_rootFsPath.c_str();
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

    // N.B. Plan9 device is always added during serialization

    systemSettings.VirtualMachine = std::move(vmSettings);
    auto json =  wsl::shared::ToJsonW(systemSettings);

    WSL_LOG("CreateLSWVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    auto idString = wsl::shared::string::GuidToString<wchar_t>(m_vmId);
    m_computeSystem = hcs::CreateComputeSystem(idString.c_str(), json.c_str());
}

HRESULT LSWVirtualMachine::GetState()
{
    return S_OK;
}