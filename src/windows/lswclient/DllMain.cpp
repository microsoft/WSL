/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file the entrypoint for the LSW client library.

--*/

#include "precomp.h"
#include "wslservice.h"
#include "LSWApi.h"
#include "wslrelay.h"

class DECLSPEC_UUID("7BC4E198-6531-4FA6-ADE2-5EF3D2A04DFF") CallbackInstance
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback, IFastRundown>
{

public:
    CallbackInstance(VirtualMachineTerminationCallback callback, void* context) : m_callback(callback), m_context(context)
    {
    }

    HRESULT OnTermination(ULONG Reason, LPCWSTR Details) override
    {
        return m_callback(m_context, static_cast<VirtualMachineTerminationReason>(Reason), Details);
    }

private:
    VirtualMachineTerminationCallback m_callback = nullptr;
    void* m_context = nullptr;
};

HRESULT WslGetVersion(WSL_VERSION_INFORMATION* Version)
try
{
    wil::com_ptr<ILSWUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(LSWUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    static_assert(sizeof(WSL_VERSION_INFORMATION) == sizeof(WSL_VERSION));

    return session->GetVersion(reinterpret_cast<WSL_VERSION*>(Version));
}
CATCH_RETURN();

HRESULT WslCreateVirtualMachine(const VirtualMachineSettings* UserSettings, LSWVirtualMachineHandle* VirtualMachine)
try
{
    wil::com_ptr<ILSWUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(LSWUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    wil::com_ptr<ILSWVirtualMachine> virtualMachineInstance;

    VIRTUAL_MACHINE_SETTINGS settings{};
    settings.DisplayName = UserSettings->DisplayName;
    settings.MemoryMb = UserSettings->Memory.MemoryMb;
    settings.CpuCount = UserSettings->CPU.CpuCount;
    settings.BootTimeoutMs = UserSettings->Options.BootTimeoutMs;
    settings.DmesgOutput = HandleToULong(UserSettings->Options.Dmesg);
    settings.EnableDebugShell = UserSettings->Options.EnableDebugShell;
    settings.NetworkingMode = UserSettings->Networking.Mode;
    settings.EnableDnsTunneling = UserSettings->Networking.DnsTunneling;

    THROW_IF_FAILED(session->CreateVirtualMachine(&settings, &virtualMachineInstance));

    wil::com_ptr_nothrow<IClientSecurity> clientSecurity;
    THROW_IF_FAILED(virtualMachineInstance->QueryInterface(IID_PPV_ARGS(&clientSecurity)));

    // Get the current proxy blanket settings.
    DWORD authnSvc, authzSvc, authnLvl, capabilites;
    THROW_IF_FAILED(clientSecurity->QueryBlanket(virtualMachineInstance.get(), &authnSvc, &authzSvc, NULL, &authnLvl, NULL, NULL, &capabilites));

    // Make sure that dynamic cloaking is used.
    WI_ClearFlag(capabilites, EOAC_STATIC_CLOAKING);
    WI_SetFlag(capabilites, EOAC_DYNAMIC_CLOAKING);
    THROW_IF_FAILED(clientSecurity->SetBlanket(
        virtualMachineInstance.get(), authnSvc, authzSvc, NULL, authnLvl, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, capabilites));

    // Register termination callback, if specified
    if (UserSettings->Options.TerminationCallback != nullptr)
    {
        auto callbackInstance =
            wil::MakeOrThrow<CallbackInstance>(UserSettings->Options.TerminationCallback, UserSettings->Options.TerminationContext);

        THROW_IF_FAILED(virtualMachineInstance->RegisterCallback(callbackInstance.Get()));

        // Callback instance is now owned by the service.
    }

    *reinterpret_cast<ILSWVirtualMachine**>(VirtualMachine) = virtualMachineInstance.detach();
    return S_OK;
}
CATCH_RETURN();

HRESULT WslAttachDisk(LSWVirtualMachineHandle VirtualMachine, const DiskAttachSettings* Settings, AttachedDiskInformation* AttachedDisk)
{
    wil::unique_cotaskmem_ansistring device;
    RETURN_IF_FAILED(reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->AttachDisk(Settings->WindowsPath, Settings->ReadOnly, &device));

    // TODO: wire LUN
    auto deviceSize = strlen(device.get());
    WI_VERIFY(deviceSize < sizeof(AttachedDiskInformation::Device));

    strncpy(AttachedDisk->Device, device.get(), sizeof(AttachedDiskInformation::Device));

    return S_OK;
}

HRESULT WslMount(LSWVirtualMachineHandle VirtualMachine, const MountSettings* Settings)
{
    return reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)
        ->Mount(Settings->Device, Settings->Target, Settings->Type, Settings->Options, Settings->Flags);
}

HRESULT WslCreateLinuxProcess(LSWVirtualMachineHandle VirtualMachine, CreateProcessSettings* UserSettings, int32_t* Pid)
{
    LSW_CREATE_PROCESS_OPTIONS options{};

    auto Count = [](const auto* Ptr) -> ULONG {
        if (Ptr == nullptr)
        {
            return 0;
        }

        ULONG Result = 0;

        while (*Ptr != nullptr)
        {
            Result++;
            Ptr++;
        }

        return Result;
    };

    options.Executable = UserSettings->Executable;
    options.CommandLine = UserSettings->Arguments;
    options.CommandLineCount = Count(options.CommandLine);
    options.Environmnent = UserSettings->Environment;
    options.EnvironmnentCount = Count(options.Environmnent);
    options.CurrentDirectory = UserSettings->CurrentDirectory;

    LSW_CREATE_PROCESS_RESULT result{};

    std::vector<LSW_PROCESS_FD> inputFd(UserSettings->FdCount);
    for (size_t i = 0; i < UserSettings->FdCount; i++)
    {
        inputFd[i] = {UserSettings->FileDescriptors[i].Number, UserSettings->FileDescriptors[i].Type};
    }

    std::vector<HANDLE> fds(UserSettings->FdCount);
    if (fds.empty())
    {
        fds.resize(1); // COM doesn't like null pointers.
    }

    RETURN_IF_FAILED(reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)
                         ->CreateLinuxProcess(&options, UserSettings->FdCount, inputFd.data(), fds.data(), &result));

    for (size_t i = 0; i < UserSettings->FdCount; i++)
    {
        UserSettings->FileDescriptors[i].Handle = fds[i];
    }

    *Pid = result.Pid;

    return S_OK;
}

HRESULT WslWaitForLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, WaitResult* Result)
{
    static_assert(ProcessStateUnknown == LSWProcessStateUnknown);
    static_assert(ProcessStateRunning == LSWProcessStateRunning);
    static_assert(ProcessStateExited == LSWProcessStateExited);
    static_assert(ProcessStateSignaled == LSWProcessStateSignaled);
    static_assert(sizeof(ProcessState) == sizeof(LSWProcessState));
    static_assert(sizeof(ProcessState) == sizeof(ULONG));

    return reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)
        ->WaitPid(Pid, TimeoutMs, reinterpret_cast<ULONG*>(&Result->State), &Result->Code);
}

HRESULT WslSignalLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal)
{
    return reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->Signal(Pid, Signal);
}

HRESULT WslShutdownVirtualMachine(LSWVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs)
{
    return reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->Shutdown(TimeoutMs);
}

void WslReleaseVirtualMachine(LSWVirtualMachineHandle VirtualMachine)
{
    reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->Release();
}

HRESULT WslLaunchInteractiveTerminal(HANDLE Input, HANDLE Output, HANDLE* Process)
try
{
    wsl::windows::common::helpers::SetHandleInheritable(Input);
    wsl::windows::common::helpers::SetHandleInheritable(Output);

    auto basePath = wsl::windows::common::wslutil::GetMsiPackagePath();
    THROW_HR_IF(E_UNEXPECTED, !basePath.has_value());

    auto commandLine = std::format(
        L"{}/wslrelay.exe --mode {} --input {} --output {}",
        basePath.value(),
        static_cast<int>(wslrelay::RelayMode::InteractiveConsoleRelay),
        HandleToULong(Input),
        HandleToUlong(Output));

    WSL_LOG("LaunchWslRelay", TraceLoggingValue(commandLine.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, commandLine.c_str()};
    process.InheritHandle(Input);
    process.InheritHandle(Output);
    process.SetFlags(CREATE_NEW_CONSOLE);
    process.SetShowWindow(SW_SHOW);
    *Process = process.Start().release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WslLaunchDebugShell(LSWVirtualMachineHandle VirtualMachine, HANDLE* Process)
try
{
    wil::unique_cotaskmem_string pipePath;
    THROW_IF_FAILED(reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->GetDebugShellPipe(&pipePath));

    wil::unique_hfile pipe{CreateFileW(pipePath.get(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    THROW_LAST_ERROR_IF(!pipe);

    wsl::windows::common::helpers::SetHandleInheritable(pipe.get());

    auto basePath = wsl::windows::common::wslutil::GetMsiPackagePath();
    THROW_HR_IF(E_UNEXPECTED, !basePath.has_value());
    auto commandLine = std::format(
        L"\"{}\\wslrelay.exe\" --mode {} --input {} --output {}",
        basePath.value(),
        static_cast<int>(wslrelay::RelayMode::InteractiveConsoleRelay),
        HandleToULong(pipe.get()),
        HandleToUlong(pipe.get()));

    WSL_LOG("LaunchDebugShellRelay", TraceLoggingValue(commandLine.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, commandLine.c_str()};
    process.InheritHandle(pipe.get());
    process.SetFlags(0);
    process.SetShowWindow(SW_SHOW);

    *Process = process.Start().release();

    return S_OK;
}
CATCH_RETURN();

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        WslTraceLoggingInitialize(LxssTelemetryProvider, false);
        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}