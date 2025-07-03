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

HRESULT GetWslVersion(WSL_VERSION* Version)
try
{
    wil::com_ptr<ILSWUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(LSWUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    return session->GetVersion(Version);
}
CATCH_RETURN();

HRESULT CreateVirualMachine(const VirtualMachineSettings* UserSettings, LSWVirtualMachineHandle* VirtualMachine)
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

    *reinterpret_cast<ILSWVirtualMachine**>(VirtualMachine) = virtualMachineInstance.detach();
    return S_OK;
}
CATCH_RETURN();

HRESULT AttachDisk(LSWVirtualMachineHandle* VirtualMachine, const DiskAttachSettings* Settings, AttachedDiskInformation* AttachedDisk)
{
    wil::unique_cotaskmem_ansistring device;
    RETURN_IF_FAILED(reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->AttachDisk(Settings->WindowsPath, Settings->ReadOnly, &device));

    // TODO: wire LUN
    auto deviceSize = strlen(device.get());
    WI_VERIFY(deviceSize < sizeof(AttachedDiskInformation::Device));

    strncpy(AttachedDisk->Device, device.get(), sizeof(AttachedDiskInformation::Device));

    return S_OK;
}

HRESULT Mount(LSWVirtualMachineHandle* VirtualMachine, const MountSettings* Settings)
{
    return reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)
        ->Mount(Settings->Device, Settings->Target, Settings->Type, Settings->Options, Settings->Chroot);
}

HRESULT CreateLinuxProcess(LSWVirtualMachineHandle* VirtualMachine, CreateProcessSettings* UserSettings, LinuxProcess* Process)
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

    static_assert(sizeof(LSW_PROCESS_FD) == sizeof(ProcessFileDescriptorSettings));

    LSW_CREATE_PROCESS_RESULT result{};

    wil::unique_cotaskmem_array_ptr<LSW_PROCESS_FD> fds;

    RETURN_IF_FAILED(reinterpret_cast<ILSWVirtualMachine*>(VirtualMachine)->CreateLinuxProcess(&options, fds.size_address<ULONG>(), &fds, &result));


    assert(fds.size() == 3);
    for (size_t i = 0; i < fds.size(); i++)
    {
        UserSettings->FileDescriptors[i].Handle = fds[i].Handle;
    }

    return S_OK;
}

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