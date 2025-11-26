/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccomm.cpp

Abstract:

    This file contains function definitions for the SvcComm helper class.

--*/

#include "precomp.h"
#include "svccomm.hpp"
#include "registry.hpp"
#include "relay.hpp"

#pragma hdrstop

//
// Macros to test exit status (defined in sys\wait.h).
//

#define LXSS_WEXITSTATUS(_status) ((_status) >> 8)
#define LXSS_WSTATUS(_status) ((_status) & 0x7F)
#define LXSS_WIFEXITED(_status) (LXSS_WSTATUS((_status)) == 0)

#define IS_VALID_HANDLE(_handle) ((_handle != NULL) && (_handle != INVALID_HANDLE_VALUE))

using wsl::windows::common::ClientExecutionContext;
namespace {

BOOL GetNextCharacter(_In_ INPUT_RECORD* InputRecord, _Out_ PWCHAR NextCharacter);
BOOL IsActionableKey(_In_ PKEY_EVENT_RECORD KeyEvent);
void SpawnWslHost(_In_ HANDLE ServerPort, _In_ const GUID& DistroId, _In_opt_ LPCGUID VmId);

struct CreateProcessArguments
{
    CreateProcessArguments(LPCWSTR Filename, int Argc, LPCWSTR Argv[], ULONG LaunchFlags, LPCWSTR WorkingDirectory)
    {
        // Populate the current working directory.
        //
        // N.B. Failure to get the current working directory is non-fatal.
        if (ARGUMENT_PRESENT(WorkingDirectory))
        {
            // If a current working directory was provided, it must be a Linux-style path.
            WI_ASSERT(*WorkingDirectory == L'/' || *WorkingDirectory == L'~');

            CurrentWorkingDirectory = WorkingDirectory;
        }
        else
        {
            LOG_IF_FAILED(wil::GetCurrentDirectoryW(CurrentWorkingDirectory));
        }

        // Populate the command line and file name.
        //
        // N.B. The CommandLineStrings vector contains weak references to the
        //      strings in the CommandLine vector.
        if (Argc > 0)
        {
            CommandLine.reserve(Argc);
            std::transform(Argv, Argv + Argc, std::back_inserter(CommandLine), [](LPCWSTR Arg) {
                return wsl::shared::string::WideToMultiByte(Arg);
            });

            CommandLineStrings.reserve(CommandLine.size());
            std::transform(CommandLine.cbegin(), CommandLine.cend(), std::back_inserter(CommandLineStrings), [](const std::string& string) {
                return string.c_str();
            });
        }

        if (ARGUMENT_PRESENT(Filename))
        {
            FilenameString = wsl::shared::string::WideToMultiByte(Filename);
        }

        // Query the current NT %PATH% environment variable.
        //
        // N.B. Failure to query the path is non-fatal.
        LOG_IF_FAILED(wil::ExpandEnvironmentStringsW(L"%PATH%", NtPath));

        if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_TRANSLATE_ENVIRONMENT))
        {
            NtEnvironment.reset(GetEnvironmentStringsW());

            // Calculate the size of the environment block.
            for (PCWSTR Variable = NtEnvironment.get(); Variable[0] != '\0';)
            {
                const size_t Length = wcslen(Variable) + 1;
                NtEnvironmentLength += Length;
                Variable += Length;
            }

            NtEnvironmentLength += 1;
        }
    }

    std::vector<std::string> CommandLine{};
    std::vector<LPCSTR> CommandLineStrings{};
    std::wstring CurrentWorkingDirectory{};
    std::string FilenameString{};
    wsl::windows::common::helpers::unique_environment_strings NtEnvironment;
    size_t NtEnvironmentLength{};
    std::wstring NtPath{};
};

void InitializeInterop(_In_ HANDLE ServerPort, _In_ const GUID& DistroId)
{
    //
    // Create a thread to handle interop requests.
    //

    wil::unique_handle WorkerThreadSeverPort{wsl::windows::common::helpers::DuplicateHandle(ServerPort)};
    std::thread([WorkerThreadSeverPort = std::move(WorkerThreadSeverPort)]() mutable {
        wsl::windows::common::wslutil::SetThreadDescription(L"Interop");
        wsl::windows::common::interop::WorkerThread(std::move(WorkerThreadSeverPort));
    }).detach();

    //
    // Spawn wslhost to handle interop requests from processes that have
    // been backgrounded and their console window has been closed.
    //

    SpawnWslHost(ServerPort, DistroId, nullptr);
}

void SpawnWslHost(_In_ HANDLE ServerPort, _In_ const GUID& DistroId, _In_opt_ LPCGUID VmId)
{
    wsl::windows::common::helpers::SetHandleInheritable(ServerPort);
    const auto RegistrationComplete = wil::unique_event(wil::EventOptions::None);
    const wil::unique_handle ParentProcess{wsl::windows::common::helpers::DuplicateHandle(GetCurrentProcess(), 0, TRUE)};
    THROW_LAST_ERROR_IF(!ParentProcess);

    const wil::unique_handle Process{wsl::windows::common::helpers::LaunchInteropServer(
        &DistroId, ServerPort, RegistrationComplete.get(), ParentProcess.get(), VmId)};

    // Wait for either the child to exit, or the registration complete event to be set.
    const HANDLE WaitHandles[] = {Process.get(), RegistrationComplete.get()};
    const DWORD WaitStatus = WaitForMultipleObjects(RTL_NUMBER_OF(WaitHandles), WaitHandles, FALSE, INFINITE);
    LOG_HR_IF_MSG(E_FAIL, (WaitStatus == WAIT_OBJECT_0), "wslhost failed to register");
}
} // namespace

//
// Exported function definitions.
//

wsl::windows::common::SvcComm::SvcComm()
{
    // Ensure that the OS has support for running lifted WSL. This interface is always present on Windows 11 and later.
    //
    // Prior to Windows 11 there are two cases where the IWslSupport interface may not be present:
    //     1. The machine has not installed the DCR that contains support for lifted WSL.
    //     2. The WSL optional component which contains the interface is not installed.
    if (!wsl::windows::common::helpers::IsWindows11OrAbove() && !wsl::windows::common::helpers::IsWslSupportInterfacePresent())
    {
        THROW_HR(wsl::windows::common::helpers::IsWslOptionalComponentPresent() ? WSL_E_OS_NOT_SUPPORTED : WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED);
    }

    auto retry_pred = []() {
        const auto errorCode = wil::ResultFromCaughtException();

        return errorCode == HRESULT_FROM_WIN32(ERROR_SERVICE_DOES_NOT_EXIST) || errorCode == REGDB_E_CLASSNOTREG;
    };

    wsl::shared::retry::RetryWithTimeout<void>(
        [this]() { m_userSession = wil::CoCreateInstance<LxssUserSession, ILxssUserSession>(CLSCTX_LOCAL_SERVER); },
        std::chrono::seconds(1),
        std::chrono::minutes(1),
        retry_pred);

    // Query client security interface.
    auto clientSecurity = m_userSession.query<IClientSecurity>();

    // Get the current proxy blanket settings.
    DWORD authnSvc, authzSvc, authnLvl, capabilities;
    THROW_IF_FAILED(clientSecurity->QueryBlanket(m_userSession.get(), &authnSvc, &authzSvc, NULL, &authnLvl, NULL, NULL, &capabilities));

    // Make sure that dynamic cloaking is used.
    WI_ClearFlag(capabilities, EOAC_STATIC_CLOAKING);
    WI_SetFlag(capabilities, EOAC_DYNAMIC_CLOAKING);
    THROW_IF_FAILED(clientSecurity->SetBlanket(
        m_userSession.get(), authnSvc, authzSvc, NULL, authnLvl, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, capabilities));
}

wsl::windows::common::SvcComm::~SvcComm()
{
}

void wsl::windows::common::SvcComm::ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->ConfigureDistribution(DistroGuid, DefaultUid, Flags, context.OutError()));
}

void wsl::windows::common::SvcComm::CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags)
{
    ClientExecutionContext context;
    THROW_IF_FAILED(CreateInstanceNoThrow(DistroGuid, Flags, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::CreateInstanceNoThrow(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) const
{
    return m_userSession->CreateInstance(DistroGuid, Flags, Error);
}

std::vector<LXSS_ENUMERATE_INFO> wsl::windows::common::SvcComm::EnumerateDistributions() const
{
    ExecutionContext enumerateDistroContext(Context::EnumerateDistros);
    ClientExecutionContext context;

    wil::unique_cotaskmem_array_ptr<LXSS_ENUMERATE_INFO> Distributions;
    THROW_IF_FAILED(m_userSession->EnumerateDistributions(Distributions.size_address<ULONG>(), &Distributions, context.OutError()));

    std::vector<LXSS_ENUMERATE_INFO> DistributionList;
    for (size_t Index = 0; Index < Distributions.size(); Index += 1)
    {
        DistributionList.push_back(Distributions[Index]);
    }

    return DistributionList;
}

HRESULT
wsl::windows::common::SvcComm::ExportDistribution(_In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    HRESULT result = E_FAIL;
    if (GetFileType(FileHandle) != FILE_TYPE_PIPE)
    {
        result = m_userSession->ExportDistribution(DistroGuid, FileHandle, stdErrWrite.get(), Flags, context.OutError());
    }
    else
    {
        result = m_userSession->ExportDistributionPipe(DistroGuid, FileHandle, stdErrWrite.get(), Flags, context.OutError());
    }

    stdErrWrite.reset();
    stdErrRelay.Sync();

    RETURN_HR(result);
}

void wsl::windows::common::SvcComm::GetDistributionConfiguration(
    _In_opt_ LPCGUID DistroGuid,
    _Out_ LPWSTR* Name,
    _Out_ ULONG* Version,
    _Out_ ULONG* DefaultUid,
    _Out_ ULONG* DefaultEnvironmentCount,
    _Out_ LPSTR** DefaultEnvironment,
    _Out_ ULONG* Flags) const
{
    ClientExecutionContext context;

    THROW_IF_FAILED(m_userSession->GetDistributionConfiguration(
        DistroGuid, Name, Version, DefaultUid, DefaultEnvironmentCount, DefaultEnvironment, Flags, context.OutError()));
}

DWORD
wsl::windows::common::SvcComm::LaunchProcess(
    _In_opt_ LPCGUID DistroGuid,
    _In_opt_ LPCWSTR Filename,
    _In_ int Argc,
    _In_reads_(Argc) LPCWSTR Argv[],
    _In_ ULONG LaunchFlags,
    _In_opt_ PCWSTR Username,
    _In_opt_ PCWSTR CurrentWorkingDirectory,
    _In_ DWORD Timeout) const
{
    ClientExecutionContext context;

    //
    // Parse the input arguments.
    //

    DWORD ExitCode = 1;
    CreateProcessArguments Parsed(Filename, Argc, Argv, LaunchFlags, CurrentWorkingDirectory);

    //
    // Create the process.
    //

    SvcCommIo Io;
    PLXSS_STD_HANDLES StdHandles = Io.GetStdHandles();
    COORD WindowSize = Io.GetWindowSize();
    ULONG Flags = LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE;
    if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_USE_SYSTEM_DISTRO))
    {
        WI_SetFlag(Flags, LXSS_CREATE_INSTANCE_FLAGS_USE_SYSTEM_DISTRO);
    }

    if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_SHELL_LOGIN))
    {
        WI_SetFlag(Flags, LXSS_CREATE_INSTANCE_FLAGS_SHELL_LOGIN);
    }

    // This method is also used by Terminal.
    // See: https://github.com/microsoft/terminal/blob/ec434e3fba2a6ef254123e31f5257c25b04f2547/src/tools/ConsoleBench/conhost.cpp#L159-L164
    HANDLE console = NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters->Reserved2[0];

    GUID DistributionId;
    GUID InstanceId;
    wil::unique_handle ProcessHandle;
    wil::unique_handle ServerPortHandle;
    wil::unique_handle StdInSocket;
    wil::unique_handle StdOutSocket;
    wil::unique_handle StdErrSocket;
    wil::unique_handle ControlSocket;
    wil::unique_handle InteropSocket;

    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR)
    {
        context.EnableInteractiveWarnings();
    }

    THROW_IF_FAILED(m_userSession->CreateLxProcess(
        DistroGuid,
        Parsed.FilenameString.empty() ? nullptr : Parsed.FilenameString.c_str(),
        Argc,
        Parsed.CommandLineStrings.data(),
        Parsed.CurrentWorkingDirectory.empty() ? nullptr : Parsed.CurrentWorkingDirectory.c_str(),
        Parsed.NtPath.empty() ? nullptr : Parsed.NtPath.c_str(),
        Parsed.NtEnvironment.get(),
        static_cast<ULONG>(Parsed.NtEnvironmentLength),
        Username,
        WindowSize.X,
        WindowSize.Y,
        HandleToUlong(console),
        StdHandles,
        Flags,
        &DistributionId,
        &InstanceId,
        &ProcessHandle,
        &ServerPortHandle,
        &StdInSocket,
        &StdOutSocket,
        &StdErrSocket,
        &ControlSocket,
        &InteropSocket,
        context.OutError()));

    context.FlushWarnings();

    WI_ASSERT((!ARGUMENT_PRESENT(DistroGuid)) || (IsEqualGUID(*DistroGuid, DistributionId)));

    //
    // If a process handle was returned, this is a WSL process. Otherwise, the
    // process is running in a utility VM.
    //

    if (ProcessHandle)
    {
        //
        // Mark the process handle as uninheritable.
        //

        helpers::SetHandleInheritable(ProcessHandle.get(), false);

        //
        // If the caller requested interop and a server port was created, start
        // the interop worker thread and background wslhost process.
        //

        if ((WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_ENABLE_INTEROP)) && (ServerPortHandle))
        {
            try
            {
                InitializeInterop(ServerPortHandle.get(), DistributionId);
            }
            CATCH_LOG()
        }

        ServerPortHandle.reset();

        //
        // Wait for the launched process to exit and return the process exit
        // code.
        //

        LXBUS_IPC_LX_PROCESS_WAIT_FOR_TERMINATION_PARAMETERS Parameters{};
        Parameters.Input.TimeoutMs = Timeout;
        THROW_IF_NTSTATUS_FAILED(LxBusClientWaitForLxProcess(ProcessHandle.get(), &Parameters));

        if (LXSS_WIFEXITED(Parameters.Output.ExitStatus))
        {
            Parameters.Output.ExitStatus = LXSS_WEXITSTATUS(Parameters.Output.ExitStatus);
        }

        ExitCode = Parameters.Output.ExitStatus;
    }
    else
    {
        //
        // Create stdin, stdout and stderr worker threads.
        //

        std::thread StdOutWorker;
        std::thread StdErrWorker;
        auto ExitEvent = wil::unique_event(wil::EventOptions::ManualReset);
        auto outWorkerExit = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&StdOutWorker, &StdErrWorker, &ExitEvent] {
            ExitEvent.SetEvent();
            if (StdOutWorker.joinable())
            {
                StdOutWorker.join();
            }

            if (StdErrWorker.joinable())
            {
                StdErrWorker.join();
            }
        });

        // This channel needs to be a shared_ptr because closing it will cause the linux relay to exit so we should keep it open
        // even after the stdin thread exits, but we can't keep give a simple reference to that thread because the main thread
        // might return from this method before the stdin relay thread does.

        auto ControlChannel = std::make_shared<wsl::shared::SocketChannel>(
            wil::unique_socket{reinterpret_cast<SOCKET>(ControlSocket.release())}, "Control");

        auto StdIn = GetStdHandle(STD_INPUT_HANDLE);
        if (IS_VALID_HANDLE(StdIn))
        {
            std::thread([StdIn, StdInSocket = std::move(StdInSocket), ControlChannel = ControlChannel, ExitHandle = ExitEvent.get(), Io = &Io]() mutable {
                auto updateTerminal = [&]() {
                    //
                    // Query the window size and send an update message via the
                    // control channel.
                    //
                    if (ControlChannel)
                    {
                        auto WindowSize = Io->GetWindowSize();

                        LX_INIT_WINDOW_SIZE_CHANGED WindowSizeMessage{};
                        WindowSizeMessage.Header.MessageType = LxInitMessageWindowSizeChanged;
                        WindowSizeMessage.Header.MessageSize = sizeof(WindowSizeMessage);
                        WindowSizeMessage.Columns = WindowSize.X;
                        WindowSizeMessage.Rows = WindowSize.Y;

                        try
                        {
                            ControlChannel->SendMessage(WindowSizeMessage);
                        }
                        CATCH_LOG();
                    }
                };

                wsl::windows::common::relay::StandardInputRelay(StdIn, StdInSocket.get(), updateTerminal, ExitHandle);
            }).detach();
        }

        auto StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        StdOutWorker = relay::CreateThread(std::move(StdOutSocket), IS_VALID_HANDLE(StdOut) ? StdOut : nullptr);
        auto StdErr = GetStdHandle(STD_ERROR_HANDLE);
        StdErrWorker = relay::CreateThread(std::move(StdErrSocket), IS_VALID_HANDLE(StdErr) ? StdErr : nullptr);

        //
        // Spawn wslhost to handle interop requests from processes that have
        // been backgrounded and their console window has been closed.
        //

        if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_ENABLE_INTEROP))
        {
            try
            {
                SpawnWslHost(InteropSocket.get(), DistributionId, &InstanceId);
            }
            CATCH_LOG()
        }

        //
        // Begin reading messages from the utility vm.
        //

        wsl::shared::SocketChannel InteropChannel{
            wil::unique_socket{reinterpret_cast<SOCKET>(InteropSocket.release())}, "Interop"};
        ExitCode = interop::VmModeWorkerThread(InteropChannel, InstanceId);
    }

    return ExitCode;
}

GUID wsl::windows::common::SvcComm::GetDefaultDistribution() const
{
    ClientExecutionContext context;
    GUID DistroId;
    THROW_IF_FAILED(m_userSession->GetDefaultDistribution(context.OutError(), &DistroId));

    return DistroId;
}

ULONG
wsl::windows::common::SvcComm::GetDistributionFlags(_In_opt_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;

    wil::unique_cotaskmem_string Name;
    ULONG Version;
    ULONG Uid;
    wil::unique_cotaskmem_array_ptr<wil::unique_cotaskmem_ansistring> Environment;
    ULONG Flags;
    THROW_IF_FAILED(m_userSession->GetDistributionConfiguration(
        DistroGuid, &Name, &Version, &Uid, Environment.size_address<ULONG>(), &Environment, &Flags, context.OutError()));

    return Flags;
}

GUID wsl::windows::common::SvcComm::GetDistributionId(_In_ LPCWSTR Name, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    GUID DistroId;
    THROW_IF_FAILED(m_userSession->GetDistributionId(Name, Flags, context.OutError(), &DistroId));

    return DistroId;
}

GUID wsl::windows::common::SvcComm::ImportDistributionInplace(_In_ LPCWSTR Name, _In_ LPCWSTR VhdPath) const
{
    ClientExecutionContext context;

    GUID DistroGuid;
    THROW_IF_FAILED(m_userSession->ImportDistributionInplace(Name, VhdPath, context.OutError(), &DistroGuid));

    return DistroGuid;
}

void wsl::windows::common::SvcComm::MoveDistribution(_In_ const GUID& DistroGuid, _In_ LPCWSTR Location) const
{
    ClientExecutionContext context;

    THROW_IF_FAILED(m_userSession->MoveDistribution(&DistroGuid, Location, context.OutError()));
}

std::pair<GUID, wil::unique_cotaskmem_string> wsl::windows::common::SvcComm::RegisterDistribution(
    _In_ LPCWSTR Name,
    _In_ ULONG Version,
    _In_ HANDLE FileHandle,
    _In_ LPCWSTR TargetDirectory,
    _In_ ULONG Flags,
    _In_ std::optional<uint64_t> VhdSize,
    _In_opt_ LPCWSTR PackageFamilyName) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    GUID DistroGuid{};
    HRESULT Result = E_FAIL;
    wil::unique_cotaskmem_string installedName;
    if (GetFileType(FileHandle) != FILE_TYPE_PIPE)
    {
        Result = m_userSession->RegisterDistribution(
            Name,
            Version,
            FileHandle,
            stdErrWrite.get(),
            TargetDirectory,
            Flags,
            VhdSize.value_or(0),
            PackageFamilyName,
            &installedName,
            context.OutError(),
            &DistroGuid);
    }
    else
    {
        Result = m_userSession->RegisterDistributionPipe(
            Name,
            Version,
            FileHandle,
            stdErrWrite.get(),
            TargetDirectory,
            Flags,
            VhdSize.value_or(0),
            PackageFamilyName,
            &installedName,
            context.OutError(),
            &DistroGuid);
    }

    stdErrWrite.reset();
    stdErrRelay.Sync();

    THROW_IF_FAILED(Result);

    return std::make_pair(DistroGuid, std::move(installedName));
}

void wsl::windows::common::SvcComm::SetDefaultDistribution(_In_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->SetDefaultDistribution(DistroGuid, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOL Sparse, _In_ BOOL AllowUnsafe) const
{
    ClientExecutionContext context;

    RETURN_HR(m_userSession->SetSparse(DistroGuid, Sparse, AllowUnsafe, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ ULONG64 NewSize) const
{
    ClientExecutionContext context;

    wil::unique_handle outputRead;
    wil::unique_handle outputWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&outputRead, &outputWrite, nullptr, 0));

    relay::ScopedRelay outputRelay(
        std::move(outputRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&outputWrite]() { outputWrite.reset(); });

    const auto result = m_userSession->ResizeDistribution(DistroGuid, outputWrite.get(), NewSize, context.OutError());

    outputWrite.reset();
    outputRelay.Sync();

    RETURN_HR(result);
}

HRESULT
wsl::windows::common::SvcComm::SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    RETURN_HR(m_userSession->SetVersion(DistroGuid, Version, stdErrWrite.get(), context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    RETURN_HR(m_userSession->AttachDisk(Disk, Flags, context.OutError()));
}

std::pair<int, int> wsl::windows::common::SvcComm::DetachDisk(_In_opt_ LPCWSTR Disk) const
{
    ClientExecutionContext context;

    int Result = -1;
    int Step = 0;
    THROW_IF_FAILED(m_userSession->DetachDisk(Disk, &Result, &Step, context.OutError()));

    return std::make_pair(Result, Step);
}

wsl::windows::common::SvcComm::MountResult wsl::windows::common::SvcComm::MountDisk(
    _In_ LPCWSTR Disk, _In_ ULONG Flags, _In_ ULONG PartitionIndex, _In_opt_ LPCWSTR Name, _In_opt_ LPCWSTR Type, _In_opt_ LPCWSTR Options) const
{
    ClientExecutionContext context;

    MountResult Result;
    THROW_IF_FAILED(m_userSession->MountDisk(
        Disk, Flags, PartitionIndex, Name, Type, Options, &Result.Result, &Result.Step, &Result.MountName, context.OutError()));

    return Result;
}

void wsl::windows::common::SvcComm::Shutdown(_In_ bool Force) const
{
    THROW_IF_FAILED(m_userSession->Shutdown(Force));
}

void wsl::windows::common::SvcComm::TerminateInstance(_In_opt_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;

    //
    // If there is an instance running, terminate it.
    //

    THROW_IF_FAILED(m_userSession->TerminateDistribution(DistroGuid, context.OutError()));
}

void wsl::windows::common::SvcComm::UnregisterDistribution(_In_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->UnregisterDistribution(DistroGuid, context.OutError()));
}
