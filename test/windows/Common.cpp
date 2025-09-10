/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Common.cpp

Abstract:

    This contains common used definitions used for testing.

--*/

// includes

#include "precomp.h"
#include "Common.h"
#include "LxssDynamicFunction.h"
#include <tlhelp32.h>
#include <werapi.h>
#include <Dbghelp.h>

using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

MODULE_SETUP(ModuleSetup);
MODULE_CLEANUP(ModuleCleanup);

// Defines
#define LXSS_LOGS_DIRECTORY L"logs"
#define LXSS_TEST_DIRECTORY L"\\data\\test"
#define LXSS_TEST_LOG_SEPARATOR_CHAR L"&"
#define LXSS_DEFAULT_TIMEOUT (15 * 1000)

//
// The instance test timeout should roughly be the maximum time to start an
// instance.
//

#define LXSS_INSTANCE_TEST_TIMEOUT (3 * 1000)

//
// The watchdog timeout is set to 3 hours.
//

#define LXSS_WATCHDOG_TIMEOUT (3 * 60 * 60 * 1000)
#define LXSS_WATCHDOG_TIMEOUT_WINDOW 1000

//
// Global variables
//

static HANDLE g_OriginalStdout;
static HANDLE g_OriginalStderr;
static BOOL g_RelogEverything = TRUE;
static bool g_LogDmesgAfterEachTest = false;
static PTP_TIMER g_WatchdogTimer;
static BOOL g_VmMode;
static std::wstring g_originalConfig;
static std::wstring g_originalDefaultDistro;
std::wstring g_dumpFolder;
std::optional<std::wstring> g_dumpToolPath;
static bool g_enableWerReport = false;
static std::wstring g_pipelineBuildId;
std::wstring g_testDistroPath;

std::pair<wil::unique_handle, wil::unique_handle> CreateSubprocessPipe(bool inheritRead, bool inheritWrite, DWORD bufferSize, _In_opt_ SECURITY_ATTRIBUTES* sa)
{
    wil::unique_handle read;
    wil::unique_handle write;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&read, &write, sa, bufferSize));

    if (inheritWrite)
    {
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(write.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    }

    if (inheritRead)
    {
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    }

    return {std::move(read), std::move(write)};
}

// LxsstuLaunchWsl

DWORD
LxsstuLaunchWsl(_In_opt_ LPCWSTR Arguments, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE StandardOutput, _In_opt_ HANDLE StandardError, _In_opt_ HANDLE Token, _In_ DWORD Flags)
{
    // Launch wsl.exe to handle the operation.
    auto CommandLine = LxssGenerateWslCommandLine(Arguments);

    return LxsstuRunCommand(CommandLine.data(), StandardInput, StandardOutput, StandardError, Token, Flags);
}

DWORD
LxsstuLaunchWsl(_In_opt_ const std::wstring& Arguments, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE StandardOutput, _In_opt_ HANDLE StandardError, _In_opt_ HANDLE Token)
{
    return LxsstuLaunchWsl(Arguments.data(), StandardInput, StandardOutput, StandardError, Token);
}

// LxsstuLaunchWslAndCaptureOutput

std::pair<std::wstring, std::wstring> LxsstuLaunchWslAndCaptureOutput(
    _In_ LPCWSTR Cmd, _In_ int ExpectedExitCode, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE Token, _In_ DWORD Flags, _In_ LPCWSTR EntryPoint)

/*++

Routine Description:

    Run a WSL command and capture its output.

Arguments:

    Cmd - The command line to run.

    ExpectedExitCode - The expected exit code from the child process.

    StandardInput - Handle to the process's standard input

Return Value:

    A pair of strings with stdout and stderr output.

--*/

{

    auto CommandLine = LxssGenerateWslCommandLine(Cmd, EntryPoint);
    return LxsstuLaunchCommandAndCaptureOutput(CommandLine.data(), ExpectedExitCode, StandardInput, Token, Flags);
}

// LxssGenerateWslCommandLine

std::wstring LxssGenerateWslCommandLine(_In_opt_ LPCWSTR Arguments, _In_ LPCWSTR EntryPoint)
{
    std::wstring CommandLine;
    THROW_IF_FAILED(wil::GetSystemDirectoryW(CommandLine));

    CommandLine += L"\\";
    CommandLine += EntryPoint;
    if (ARGUMENT_PRESENT(Arguments))
    {
        CommandLine += L" ";
        CommandLine += Arguments;
    }

    return CommandLine;
}

// LxsstuLaunchWslAndCaptureOutput

std::pair<std::wstring, std::wstring> LxsstuLaunchWslAndCaptureOutput(
    _In_ const std::wstring& Cmd, _In_ int ExpectedExitCode, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE Token, _In_ DWORD Flags, _In_ LPCWSTR EntryPoint)

/*++

Routine Description:

    Run a wsl command and return its output.

Arguments:

    Cmd - Supplies the wsl command to run.

    ExpectedExitCode - The expected exit code from the child process.

    StandardInput - Handle to the process's standard input

Return Value:

    The command's stdout and stderr output.

--*/

{
    return LxsstuLaunchWslAndCaptureOutput(Cmd.data(), ExpectedExitCode, StandardInput, Token, Flags, EntryPoint);
}

std::pair<std::wstring, std::wstring> LxsstuLaunchCommandAndCaptureOutput(_In_ LPWSTR Cmd, _In_ LPCSTR StandardInput, _In_opt_ HANDLE Token, _In_ DWORD Flags)
{
    const auto inputSize = static_cast<DWORD>(strlen(StandardInput));
    auto [read, write] = CreateSubprocessPipe(true, false, inputSize);
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(write.get(), StandardInput, inputSize, nullptr, nullptr));
    write.reset();

    return LxsstuLaunchCommandAndCaptureOutput(Cmd, 0, read.get(), Token, Flags);
}

// LxsstuLaunchCommandAndCaptureOutputWithResult

std::tuple<std::wstring, std::wstring, int> LxsstuLaunchCommandAndCaptureOutputWithResult(
    _In_ LPWSTR Cmd, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE Token, _In_ DWORD Flags)

/*++

Routine Description:

    Run a command and capture its output.

Arguments:

    Cmd - The command line to run.

Return Value:

    A pair of strings with stdout and stderr output.

--*/

{

    wsl::windows::common::SubProcess process(nullptr, Cmd);
    process.SetStdHandles(StandardInput, nullptr, nullptr);
    process.SetToken(Token);
    process.SetFlags(Flags);

    auto result = process.RunAndCaptureOutput();

    return {result.Stdout, result.Stderr, result.ExitCode};
}

// LxsstuLaunchCommandAndCaptureOutput

std::pair<std::wstring, std::wstring> LxsstuLaunchCommandAndCaptureOutput(
    _In_ LPWSTR Cmd, _In_ int ExpectedExitCode, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE Token, _In_ DWORD Flags)

/*++

Routine Description:

    Run a command and capture its output.

Arguments:

    Cmd - The command line to run.

Return Value:

    A pair of strings with stdout and stderr output.

--*/

{
    auto [Out, Err, ExitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(Cmd, StandardInput, Token, Flags);
    if (ExitCode != ExpectedExitCode)
    {
        THROW_HR_MSG(
            E_UNEXPECTED,
            "Command \"%ls\""
            "returned unexpected exit code (%lu != %i). "
            "Stdout: '%ls'"
            "Stderr: '%ls'",
            Cmd,
            ExitCode,
            ExpectedExitCode,
            Out.c_str(),
            Err.c_str());
    }

    return std::make_pair(Out, Err);
}

// LxsstuRunCommand

DWORD
LxsstuRunCommand(_In_ LPWSTR Command, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE StandardOutput, _In_opt_ HANDLE StandardError, _In_opt_ HANDLE Token, _In_ DWORD Flags)
{
    const auto Process = LxsstuStartProcess(Command, StandardInput, StandardOutput, StandardError, Token, Flags);
    return wsl::windows::common::SubProcess::GetExitCode(Process.get());
}

// LxsstuStartProcess

wil::unique_handle LxsstuStartProcess(
    _In_ LPWSTR Command, _In_opt_ HANDLE StandardInput, _In_opt_ HANDLE StandardOutput, _In_opt_ HANDLE StandardError, _In_opt_ HANDLE Token, _In_ DWORD Flags)
{
    wsl::windows::common::SubProcess process(nullptr, Command);

    process.SetStdHandles(
        ARGUMENT_PRESENT(StandardInput) ? StandardInput : GetStdHandle(STD_INPUT_HANDLE),
        ARGUMENT_PRESENT(StandardOutput) ? StandardOutput : GetStdHandle(STD_OUTPUT_HANDLE),
        ARGUMENT_PRESENT(StandardError) ? StandardError : GetStdHandle(STD_ERROR_HANDLE));

    process.SetToken(Token);
    process.SetFlags(Flags);

    return process.Start();
}

// FileFromHandle

wil::unique_file FileFromHandle(_Inout_ wil::unique_handle& Handle, _In_ const char* Mode)

/*++

Routine Description:

    Create a FILE from a handle.

Arguments:
    Handle - The handle to create the FILE from.

    Mode - The mode to create the FILE with.

Return Value:

    The created FILE.

--*/

{

    using UniqueFd = wil::unique_any<int, decltype(_close), _close, wil::details::pointer_access_all, int, int, -1>;

    UniqueFd Fd(_open_osfhandle(reinterpret_cast<intptr_t>(Handle.get()), 0));
    if (Fd.get() < 0)
    {
        THROW_LAST_ERROR_MSG("_open_osfhandle failed");
    }

    Handle.release();

    wil::unique_file File(_fdopen(Fd.get(), Mode));
    VERIFY_IS_NOT_NULL(File.get());
    Fd.release();

    return File;
}

// LxsstuInitialize

BOOL LxsstuInitialize(__in BOOLEAN RunInstanceTests)
{
    wil::unique_hkey Key;
    LRESULT Result;
    BOOL Success;
    DWORD Value;

    Success = FALSE;

    THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    //
    // Don't fail if CoInitializeSecurity has already been called.
    //

    const auto Hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_STATIC_CLOAKING, 0);

    THROW_HR_IF(Hr, FAILED(Hr) && Hr != RPC_E_TOO_LATE);

    WSADATA Data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

    VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSL_UTF8", L"1"));

    if (LxsstuVmMode() == FALSE)
    {
        Result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, 0, KEY_ALL_ACCESS, &Key);

        if (Result != ERROR_SUCCESS)
        {
            LogError("RegOpenKeyEx %s failed with %Id", LXSS_REGISTRY_PATH, Result);

            goto InitializeEnd;
        }

        //
        // Set the error level to critical so the driver will not break into kd
        // while the test is running.
        //

        Value = LxErrorLevel_Critical;
        Result = RegSetValueEx(Key.get(), LX_QUERY_REGISTRY_ERROR_LEVEL_SUBKEY, 0, REG_DWORD, (const PBYTE)&Value, sizeof(DWORD));

        if (Result != ERROR_SUCCESS)
        {
            LogError("RegSetValueEx %s failed with %Id", LX_QUERY_REGISTRY_ERROR_LEVEL_SUBKEY, Result);

            goto InitializeEnd;
        }

        //
        // Disable breaking on syscall failures.
        //

        Value = FALSE;
        Result = RegSetValueEx(Key.get(), LX_QUERY_REGISTRY_BREAK_ON_SYSCALL_FAILURE_SUBKEY, 0, REG_DWORD, (const PBYTE)&Value, sizeof(DWORD));

        if (Result != ERROR_SUCCESS)
        {
            LogError("RegSetValueEx %s failed with %Id", LX_QUERY_REGISTRY_BREAK_ON_SYSCALL_FAILURE_SUBKEY, Result);

            goto InitializeEnd;
        }

        //
        // Enable lxbus root access.
        //

        Value = TRUE;
        Result = RegSetValueEx(Key.get(), LX_QUERY_REGISTRY_ROOT_LXBUS_ACCESS, 0, REG_DWORD, (const PBYTE)&Value, sizeof(DWORD));

        if (Result != ERROR_SUCCESS)
        {
            LogError("RegSetValueEx %s failed with %Id", LX_QUERY_REGISTRY_ROOT_LXBUS_ACCESS, Result);

            goto InitializeEnd;
        }

        //
        // Enable mounting DrvFs with case=force.
        //

        Value = TRUE;
        Result = RegSetValueEx(Key.get(), LX_QUERY_REGISTRY_DRVFS_ALLOW_FORCE_CASE_SENSITIVITY, 0, REG_DWORD, (const PBYTE)&Value, sizeof(DWORD));

        if (Result != ERROR_SUCCESS)
        {
            LogError("RegSetValueEx %s failed with %Id", LX_QUERY_REGISTRY_DRVFS_ALLOW_FORCE_CASE_SENSITIVITY, Result);

            goto InitializeEnd;
        }
    }
    else
    {
        const auto LogDirectory = LxsstuGetTestDirectory() + L"\\log";
        wil::CreateDirectoryDeep(LogDirectory.c_str());
    }

    //
    // Run the instance tests.
    //

    if (RunInstanceTests != FALSE)
    {
        VERIFY_NO_THROW(LxsstuInstanceTests());
    }

    Success = TRUE;

InitializeEnd:

    return Success;
}

// LxxstuVmMode

BOOL LxsstuVmMode(VOID)

/*++

Routine Description:

    Queries if the tests are being run in VM mode.

Arguments:

    None.

Return Value:

    TRUE if the tests are running in VM mode, FALSE otherwise.

--*/

{
    return g_VmMode;
}

// LxsstuLaunchPowershellAndCaptureOutput

std::pair<std::wstring, std::wstring> LxsstuLaunchPowershellAndCaptureOutput(_In_ const std::wstring& Cmd, _In_ int ExpectedExitCode)

/*++

Routine Description:

    Run a powershell command and return its output.

Arguments:

    Cmd - Supplies the powershell command to run.

    ExpectedExitCode - The expected exit code from the child process.

Return Value:
s
    The command's stdout and stderr output.

--*/

{
    auto CommandLine = L"Powershell -NoProfile -Command \"" + Cmd + L"\"";
    LogInfo("Running the command: %ls\n", CommandLine.c_str());
    return LxsstuLaunchCommandAndCaptureOutput(CommandLine.data(), ExpectedExitCode);
}

// LxsstuUninitialize

VOID LxsstuUninitialize(__in BOOLEAN RunInstanceTests)
{

    wil::unique_hkey Key;
    LRESULT Result;

    //
    // Run the instance tests again to make sure that the instance can be
    // started and stopped (i.e. no leaked fs references).
    //

    if (RunInstanceTests != FALSE)
    {
        VERIFY_NO_THROW(LxsstuInstanceTests());
    }

    if (LxsstuVmMode() == FALSE)
    {

        //
        // Delete registry subkeys that were set by the test framework.
        //

        Result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, LXSS_REGISTRY_PATH, 0, KEY_ALL_ACCESS, &Key);

        if (Result != ERROR_SUCCESS)
        {
            LogInfo("RegOpenKeyEx failed with %Id", Result);
        }
        else
        {
            auto DeleteKey = [&](LPCWSTR KeyName) {
                Result = RegDeleteKeyValue(Key.get(), nullptr, KeyName);
                if (Result != ERROR_SUCCESS)
                {
                    LogInfo("RegDeleteKeyValue %s failed with %Id", KeyName, Result);
                }
            };

            DeleteKey(LX_QUERY_REGISTRY_ERROR_LEVEL_SUBKEY);
            DeleteKey(LX_QUERY_REGISTRY_BREAK_ON_SYSCALL_FAILURE_SUBKEY);
            DeleteKey(LX_QUERY_REGISTRY_ROOT_LXBUS_ACCESS);
            DeleteKey(LX_QUERY_REGISTRY_DRVFS_ALLOW_FORCE_CASE_SENSITIVITY);
        }
    }

    VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSL_UTF8", nullptr));

    WSACleanup();

    //
    // Clear the winrt cache in case LookupLiftedPackage() is called again after another CoInitialize().
    //

    winrt::clear_factory_cache();

    CoUninitialize();

    return;
}

// LxssLogKernelOutput

void LxssLogKernelOutput(VOID)

/*++

Routine Description:

    Write the kernel output in the test logs.

Arguments:
    None.

Return Value:

    None.

--*/

{
    if (!g_LogDmesgAfterEachTest)
    {
        return;
    }

    //
    // dmesg -c isn't implemented on WSL1
    //

    const auto cmd = LxsstuVmMode() ? L"dmesg -c" : L"dmesg";
    const auto Output = LxsstuLaunchWslAndCaptureOutput(cmd);
    LogInfo("Kernel logs: '%ls'", Output.first.c_str());
}

// LxsstuGetTestDirectory

std::wstring LxsstuGetTestDirectory(VOID)

/*++

Description:

    This routine gets the test directory.

Parameters:

    None.

Return:

    The test directory.

--*/

{

    std::wstring TestDirectory = LxsstuGetLxssDirectory();
    TestDirectory += L"\\" LXSS_ROOTFS_DIRECTORY LXSS_TEST_DIRECTORY;
    return TestDirectory;
}

// LxsstuGetLxssDirectory

std::wstring LxsstuGetLxssDirectory(VOID)

/*++

Description:

    This routine gets the lxss directory.

Parameters:

    None.

Return:

    The lxss directory.

--*/

{

    const wil::unique_hkey LxssKey = wsl::windows::common::registry::OpenLxssUserKey();
    const std::wstring Default = wsl::windows::common::registry::ReadString(LxssKey.get(), nullptr, L"DefaultDistribution", nullptr);

    std::wstring BasePath = wsl::windows::common::registry::ReadString(LxssKey.get(), Default.c_str(), L"BasePath", nullptr);

    return BasePath;
}

void CaptureLiveDump()
{
    auto PrivilegeState = wsl::windows::common::security::AcquirePrivilege(SE_DEBUG_NAME);

    const std::wstring targetFile = g_dumpFolder + L"\\livedump.dmp";
    LogInfo("Writing livedump in: %ls", targetFile.c_str());

    wsl::windows::common::SubProcess dumpProcess{nullptr, std::format(L"{} \"{}\"", g_dumpToolPath->c_str(), targetFile.c_str()).c_str()};
    const auto exitCode = dumpProcess.Run();
    if (exitCode != 0)
    {
        LogError("Failed to capture livedump. ExitCode=%lu", exitCode);
        return;
    }

    LogInfo("Dump size: %llu", std::filesystem::file_size(targetFile));

    // Try to compress the dump.
    std::wstring command = L"Powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Compress-Archive -Force -Path '" +
                           targetFile + L"' -DestinationPath '" + targetFile + L".zip'\"";
    if (LxsstuRunCommand(command.data()) != 0)
    {
        // Note: powershell will fail to create the .zip if the dump is bigger than 2GB with:
        // Exception calling "Write" with "3" argument(s): "Stream was too long."
        LogError("Failed to compress live dump");
    }
    else
    {
        THROW_IF_WIN32_BOOL_FALSE(DeleteFile(targetFile.c_str()));
    }
}

DEFINE_ENUM_FLAG_OPERATORS(MINIDUMP_TYPE);

DWORD FindThreadInProcess(DWORD Pid)
{
    const wil::unique_handle Threads{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};

    THREADENTRY32 ThreadInfo{};
    ThreadInfo.dwSize = sizeof(ThreadInfo);
    for (auto result = Thread32First(Threads.get(), &ThreadInfo); result; result = Thread32Next(Threads.get(), &ThreadInfo))
    {
        if (ThreadInfo.th32OwnerProcessID == Pid)
        {
            return ThreadInfo.th32ThreadID;
        }
    }

    THROW_HR(HRESULT_FROM_WIN32(STATUS_NOT_FOUND));
}

PVOID GetModuleAddressInProcess(HANDLE Process, const std::wstring& Module)
{
    // From: https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocessmodulesex
    // Do not call CloseHandle on any of the handles returned by this function. The information comes from a snapshot, so there are no resources to be freed.

    std::vector<HMODULE> Modules;
    DWORD RequiredSize{};
    bool Result{};
    do
    {
        Modules.resize(RequiredSize / sizeof(HMODULE));
        Result = EnumProcessModulesEx(Process, Modules.data(), static_cast<DWORD>(Modules.size() * sizeof(HMODULE)), &RequiredSize, LIST_MODULES_ALL);
    } while (Result && RequiredSize / sizeof(HMODULE) > Modules.size());

    for (const auto& e : Modules)
    {
        std::filesystem::path modulePath = wil::GetModuleFileNameExW<std::wstring>(Process, e);

        if (wsl::windows::common::string::IsPathComponentEqual(modulePath.filename().native(), Module))
        {
            MODULEINFO Info{};
            THROW_IF_WIN32_BOOL_FALSE(GetModuleInformation(Process, e, &Info, sizeof(Info)));

            return Info.lpBaseOfDll;
        }
    }

    THROW_HR(HRESULT_FROM_WIN32(STATUS_NOT_FOUND));
}

void CreateCrashReport(HANDLE Process, LPCWSTR ProcessName, DWORD Pid, std::wstring const& EventName)
{
    using unique_hreport = wil::unique_any<HREPORT, decltype(WerReportCloseHandle), WerReportCloseHandle>;

    auto setProperty = [](LPWSTR Target, const std::wstring& Value, size_t BufferSize) {
        wcsncpy(Target, Value.c_str(), std::min(BufferSize - 1, Value.size()));
    };

    WER_REPORT_INFORMATION Info{};
    Info.dwSize = sizeof(Info);
    Info.hProcess = Process;

    setProperty(Info.wzDescription, EventName, ARRAYSIZE(Info.wzDescription));
    setProperty(Info.wzApplicationName, ProcessName, ARRAYSIZE(Info.wzApplicationName));
    setProperty(Info.wzApplicationPath, wil::GetModuleFileNameExW<std::wstring>(Process, nullptr), ARRAYSIZE(Info.wzApplicationPath));

    unique_hreport Report;
    THROW_IF_FAILED(WerReportCreate(EventName.c_str(), WerReportApplicationCrash, &Info, &Report));

    const std::wstring DumpPath = g_dumpFolder + L"\\" + ProcessName + L"." + std::to_wstring(Pid) + L".hdmp";
    wil::unique_hfile DumpFile{CreateFileW(DumpPath.c_str(), GENERIC_ALL, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!DumpFile);

    std::optional<MINIDUMP_EXCEPTION_INFORMATION> ExceptionInfo;
    EXCEPTION_RECORD Record{};
    EXCEPTION_POINTERS Pointers{};

    // To get access to the dumps in AzureWatson, the exception address needs to point to a module
    // that we own. To do that, load the main module and point the exception to its entrypoint.
    try
    {
        Record.ExceptionAddress = GetModuleAddressInProcess(Process, ProcessName);
        Record.ExceptionCode = EXCEPTION_BREAKPOINT;
        Pointers.ExceptionRecord = &Record;

        ExceptionInfo.emplace();
        ExceptionInfo->ExceptionPointers = &Pointers;
        ExceptionInfo->ThreadId = FindThreadInProcess(Pid);
    }
    catch (...)
    {
        LogError("Failed to find module address / thread id for %ls, 0x%x", ProcessName, wil::ResultFromCaughtException());
    }

    THROW_IF_WIN32_BOOL_FALSE(MiniDumpWriteDump(
        Process,
        Pid,
        DumpFile.get(),
        MiniDumpWithDataSegs | MiniDumpWithProcessThreadData | MiniDumpWithHandleData | MiniDumpWithPrivateReadWriteMemory |
            MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo | MiniDumpWithTokenInformation |
            MiniDumpWithPrivateWriteCopyMemory | MiniDumpWithCodeSegs,
        ExceptionInfo.has_value() ? &ExceptionInfo.value() : nullptr,
        nullptr,
        nullptr));

    DumpFile.reset();

    THROW_IF_FAILED(WerReportAddFile(Report.get(), DumpPath.c_str(), WerFileTypeHeapdump, 0));

    WER_SUBMIT_RESULT SubmitResult{};
    const auto Result = WerReportSubmit(
        Report.get(),
        WerConsentApproved,
        WER_SUBMIT_ADD_REGISTERED_DATA | WER_SUBMIT_NO_CLOSE_UI | WER_SUBMIT_BYPASS_DATA_THROTTLING | WER_SUBMIT_REPORT_MACHINE_ID | WER_SUBMIT_QUEUE,
        &SubmitResult);

    LogInfo("WerReportSubmit() returned 0x%x, SubmitResult = %i, EventName = %ls", Result, SubmitResult, EventName.c_str());
}

void CreateProcessCrashReport(DWORD Pid, LPCWSTR ImageName, LPCWSTR EventName)
{
    try
    {
        LogInfo("Opening process %ls, Pid %lu", ImageName, Pid);
        const wil::unique_handle Process(OpenProcess(PROCESS_ALL_ACCESS, FALSE, Pid));
        THROW_LAST_ERROR_IF_NULL(Process);

        CreateCrashReport(Process.get(), ImageName, Pid, EventName);
    }
    catch (...)
    {
        LogError("Failed to create crash report for process %ls (%lu), %lu", ImageName, Pid, wil::ResultFromCaughtException());
    }
}

void CreateWerReports()
{
    static const std::set<std::wstring, wsl::shared::string::CaseInsensitiveCompare> WslProcesses{
        L"wsl.exe", L"wslhost.exe", L"wslrelay.exe", L"wslservice.exe", L"wslg.exe", L"vmcompute.exe", L"vmwp.exe"};

    auto PrivilegeState = wsl::windows::common::security::AcquirePrivilege(SE_DEBUG_NAME);
    const std::wstring EventName = L"WslTestHang-" + g_pipelineBuildId;

    LogInfo("Dumps here: https://azurewatson.microsoft.com/?EventType=%s", EventName.c_str());

    // Start by capturing the test process, since collect dmesg changes the state of the UVM.
    try
    {
        CreateProcessCrashReport(GetCurrentProcessId(), L"te.processhost.exe", EventName.c_str());
    }
    CATCH_LOG();

    PROCESSENTRY32 PE32;
    PE32.dwSize = sizeof(PE32);
    const wil::unique_handle ProcessSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    THROW_LAST_ERROR_IF(ProcessSnapshot.get() == INVALID_HANDLE_VALUE);

    try
    {
        if (Process32First(ProcessSnapshot.get(), &PE32))
        {
            do
            {
                if (WslProcesses.find(std::wstring(PE32.szExeFile)) == WslProcesses.end())
                {
                    continue;
                }

                try
                {
                    CreateProcessCrashReport(PE32.th32ProcessID, PE32.szExeFile, EventName.c_str());
                }
                CATCH_LOG();

            } while (Process32Next(ProcessSnapshot.get(), &PE32));
        }
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_NO_MORE_FILES);
    }
    CATCH_LOG();

    // Also capture an HNS dump. Since the process name is svchost.exe, find its pid from its service.
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT)};
    THROW_LAST_ERROR_IF_NULL(manager);

    const wil::unique_schandle service{OpenService(manager.get(), L"HNS", SERVICE_QUERY_STATUS)};
    THROW_LAST_ERROR_IF_NULL(service);

    auto [_, pid] = GetServiceState(service.get());
    CreateProcessCrashReport(pid, L"svchost.exe", EventName.c_str());
}

void DumpGuestProcesses()
{
    constexpr auto dumpScript =
        R"(
set -ue

dmesg

# Try to install gdb
tdnf install -y gdb || true

declare -a pids_to_dump

for proc in /proc/[0-9]*; do
  read -a stats < "$proc/stat" # Skip kernel threads to make the output easier to read
  flags=${stats[8]}

  if (( ("$flags" & 0x00200000) == 0x00200000 )); then
    continue
  fi

  pid=$(basename "$proc")

  pids_to_dump+=("$pid")
  parent=$(ps -o ppid= -p "$pid")

  echo -e "\nProcess: $pid (parent: $parent) "
  echo -en "cmd: "
  cat "/proc/$pid/cmdline" || true
  echo -e "\nstat: "
  cat "/proc/$pid/stat" || true

  for tid in $(ls "/proc/$pid/task" || true); do
    echo -n "tid: $tid - "
    cat "/proc/$pid/task/$tid/comm" || true
    cat "/proc/$pid/task/$tid/stack" || true
  done

  echo "fds: "
  ls -la "/proc/$pid/fd" || true
done

for pid in "${pids_to_dump[@]}" ; do
   name=$(ps -p "$pid" -o comm=)
   if [[ "$name" =~ ^(bash|login)$ ]]; then
     echo "Skipping dump for process: $name"
     continue
   fi

   echo "Dumping process: $name ($pid) "
   if gcore -a -o core "$pid" ; then
     if ! /wsl-capture-crash 0 "$name" "$pid" 0 < "core.$pid" ; then
         echo "Failed to dump process $pid"
     fi

     rm "core.$pid" 
   fi
done

echo "hvsockets: "
ss -lap --vsock

echo "meminfo: "
cat /proc/meminfo

poweroff -f
)";

    const std::wstring filePath = g_dumpFolder + L"\\guest-state.txt";
    LogInfo("Dumping guest processes in: %ls", filePath.c_str());

    const wil::unique_hfile outputFile{CreateFileW(
        filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    THROW_LAST_ERROR_IF(!outputFile);
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(outputFile.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

    auto [readPipe, writePipe] = CreateSubprocessPipe(true, false);

    auto cmd = LxssGenerateWslCommandLine(L"--debug-shell");
    const auto process = LxsstuStartProcess(cmd.data(), readPipe.get(), outputFile.get());

    THROW_IF_WIN32_BOOL_FALSE(WriteFile(writePipe.get(), dumpScript, static_cast<DWORD>(strlen(dumpScript)), nullptr, nullptr));
    writePipe.reset();

    // Wait up to 5 minutes for that process
    const auto result = WaitForSingleObject(process.get(), 60 * 1000 * 5);
    if (result != WAIT_TIMEOUT)
    {
        LogError("Unexpected status waiting for the debug shell, %lu", result);
    }
}

// LxsstuWatchdogTimer

VOID __stdcall LxsstuWatchdogTimer(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID ThreadpoolTimerContext, _Inout_ PTP_TIMER Timer)

/*++

Routine Description:

    Runs when the watch dog timer has fired to crash the process.

Arguments:

    Instance - Not used.

    ThreadpoolTimerContext - Not used.

    Timer - Not used.

Return Value:

    None.

--*/

{

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(ThreadpoolTimerContext);
    UNREFERENCED_PARAMETER(Timer);

    try
    {
        if (g_enableWerReport)
        {
            CreateWerReports();
        }
        else
        {
            LogError("Wer reporting disabled, skipping");
        }
    }
    catch (...)
    {
        LogError("Failed to create WER report, 0x%x", wil::ResultFromCaughtException());
    }

    if (LxsstuVmMode())
    {
        try
        {
            DumpGuestProcesses();
        }
        catch (...)
        {
            LogError("Failed to dump guest processes, 0x%x", wil::ResultFromCaughtException());
        }
    }

    try
    {
        if (g_enableWerReport && g_dumpToolPath.has_value())
        {
            CaptureLiveDump();
        }
    }
    catch (...)
    {
        LogError("Failed to capture livedump, 0x%x", wil::ResultFromCaughtException());
    }

    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return;
}

// LxsstuInstanceTests

VOID LxsstuInstanceTests(VOID)

/*++

Routine Description:

    Runs the instance unit tests.

Arguments:

    None.

Return Value:

    None.

--*/

{

    ULONG Iteration;
    ULONG NumberOfIterations;
    ULONG SleepDuration;
    unsigned int Seed;

    //
    // Start and stop an instance multiple times, sleeping a random duration
    // between the start and stop.
    //

    NumberOfIterations = 5;
    Seed = GetTickCount();
    srand(Seed);
    LogInfo("Starting instance tests, Seed = %u", Seed);
    for (Iteration = 0; Iteration < NumberOfIterations; Iteration++)
    {
        LogInfo("Create instance - Iteration %u of %u", (Iteration + 1), NumberOfIterations);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"/bin/true"), 0u);
        SleepDuration = rand() % LXSS_INSTANCE_TEST_TIMEOUT;
        LogInfo("Sleeping %u milliseconds before destroying instance...", SleepDuration);

        SleepEx(SleepDuration, FALSE);
        TerminateDistribution();
    }

    LogPass("Instance tests passed");

    return;
}

// LxxsSplitString

std::vector<std::wstring> LxssSplitString(_In_ const std::wstring& String, _In_ const std::wstring& Delim)

/*++

Routine Description:

    Split a string by a delimiter.

Arguments:

    String - Supplies the string to split.

    Delim - The delimiter to split the string on.

Return Value:

    A vector of split string.

--*/

{
    std::vector<std::wstring> output;

    std::wistringstream input(String);
    std::wstring entry;

    std::string::size_type index = 0;
    std::string::size_type previous_index = 0;

    while ((index = String.find(Delim, previous_index)) != std::string::npos)
    {
        output.emplace_back(String.substr(previous_index, index - previous_index));
        previous_index = index + Delim.size();
    }

    auto remaining = String.substr(previous_index);
    if (remaining != Delim && !remaining.empty())
    {
        output.emplace_back(std::move(remaining));
    }

    return output;
}

// WslKeepAlive class definitions

WslKeepAlive::WslKeepAlive(HANDLE Token) : m_token(Token)
{
    Set();
}

WslKeepAlive::~WslKeepAlive()
{
    Reset();
}

void WslKeepAlive::Set()
{
    std::tie(m_read, m_write) = CreateSubprocessPipe(true, false);

    m_running.emplace();
    m_thread = std::thread(std::bind(&WslKeepAlive::Run, this));
    m_running->get_future().wait();
}

void WslKeepAlive::Run()
{
    try
    {
        // Create a pipe to read wsl's output
        wil::unique_handle read;
        wil::unique_handle write;
        SECURITY_ATTRIBUTES attributes = {0};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = true;
        THROW_LAST_ERROR_IF(!CreatePipe(&read, &write, &attributes, sizeof(attributes)));

        // Start a process that outputs 'running', then waits
        const std::wstring expectedOutput = L"running";
        std::wstring cmd = L"wsl.exe echo -n " + expectedOutput + L" && read -n 1 ";
        const auto process = LxsstuStartProcess(cmd.data(), m_read.get(), write.get(), nullptr, m_token);
        write.reset();

        // Wait until we read 'running'
        std::string buffer(expectedOutput.size(), '\0');
        DWORD bytesRead = 0;
        VERIFY_IS_TRUE(ReadFile(read.get(), buffer.data(), static_cast<DWORD>(expectedOutput.size()), &bytesRead, nullptr));

        VERIFY_ARE_EQUAL(buffer, wsl::shared::string::WideToMultiByte(expectedOutput));

        m_running->set_value();

        WaitForSingleObject(process.get(), INFINITE);
    }
    catch (...)
    {
        LogError("Caught exception in WslKeepAlive::Run");
        m_running->set_exception(std::current_exception());
    }
}

void WslKeepAlive::Reset()
{
    if (m_thread.joinable())
    {
        const char c = 'k';
        THROW_LAST_ERROR_IF(!WriteFile(m_write.get(), &c, sizeof(c), nullptr, nullptr));
        m_write.reset();
        m_thread.join();
    }
}

std::pair<DWORD, DWORD> GetServiceState(SC_HANDLE service)
{
    DWORD dwBytesNeeded{};
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &dwBytesNeeded))
    {
        LogError("QueryServiceStatusEx() failed, %lu", GetLastError());
        VERIFY_FAIL();
    }

    return std::make_pair(status.dwCurrentState, status.dwProcessId);
}

void WaitForServiceState(SC_HANDLE service, DWORD state, DWORD previousPid)
{
    DWORD currentState{};
    DWORD pid{};
    auto pred = [&]() {
        std::tie(currentState, pid) = GetServiceState(service);
        if (pid != previousPid && state == SERVICE_STOPPED)
        {
            return;
        }

        THROW_HR_IF(E_ABORT, currentState != state && currentState != SERVICE_STOPPED);
    };

    try
    {
        wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::milliseconds(100), std::chrono::minutes(2), [&]() {
            return wil::ResultFromCaughtException() == E_ABORT;
        });
    }
    catch (...)
    {
        LogError("Timed waiting for service to reach state: %lu. Current state: %lu, error: 0x%x", state, currentState, wil::ResultFromCaughtException());
    }
}

void StopService(SC_HANDLE service)
{
    // Some services don't accept SERVICE_CONTROL_STOP when starting.
    // Wait for them to be running before stopping them
    auto [state, pid] = GetServiceState(service);
    if (state == SERVICE_START_PENDING)
    {
        WaitForServiceState(service, SERVICE_RUNNING, pid);
    }

    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
    {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE)
        {
            LogError("Unexpected error code: 0x%x", error);
            VERIFY_FAIL();
        }
        return; // Service is not running
    }

    WaitForServiceState(service, SERVICE_STOPPED, pid);
}

void RestartWslService()
/*++

Routine Description:

    Restart the WSL service.

Arguments:

    None.

Return Value:

    None.

--*/
{
    LogInfo("Restarting WSLService");
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT)};
    VERIFY_IS_NOT_NULL(manager);

    const wil::unique_schandle service{OpenService(manager.get(), L"wslservice", SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START)};
    VERIFY_IS_NOT_NULL(service);

    StopService(service.get());
    if (!StartService(service.get(), 0, nullptr))
    {
        VERIFY_ARE_EQUAL(GetLastError(), ERROR_SERVICE_ALREADY_RUNNING);
    }
}

void StopWslService()
{
    LogInfo("Stopping WSLService");
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT)};
    VERIFY_IS_NOT_NULL(manager);

    const wil::unique_schandle service{OpenService(manager.get(), L"wslservice", SERVICE_STOP | SERVICE_QUERY_STATUS)};
    VERIFY_IS_NOT_NULL(service);
    StopService(service.get());
}

wil::unique_handle GetNonElevatedToken()
{
    const auto token = wil::open_current_access_token(TOKEN_ALL_ACCESS);

    wil::unique_handle nonElevatedToken;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &nonElevatedToken));

    wil::unique_sid mediumIntegritySid;
    THROW_LAST_ERROR_IF(!ConvertStringSidToSidA("S-1-16-8192", &mediumIntegritySid));

    TOKEN_MANDATORY_LABEL label = {0};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = mediumIntegritySid.get();
    THROW_IF_WIN32_BOOL_FALSE(SetTokenInformation(nonElevatedToken.get(), TokenIntegrityLevel, &label, sizeof(label)));

    return nonElevatedToken;
}

WslConfigChange::WslConfigChange(const std::wstring& Content)
{
    m_originalContent = Update(Content);
}

WslConfigChange::WslConfigChange(WslConfigChange&& other) : m_originalContent(std::move(other.m_originalContent))
{
}

std::wstring WslConfigChange::Update(const std::wstring& Content)
{
    auto previous = LxssWriteWslConfig(Content);

    if (previous != Content)
    {
        RestartWslService();
    }

    return previous;
}

WslConfigChange::~WslConfigChange()
{
    if (m_originalContent)
    {
        Update(m_originalContent.value());
    }
}

// writes global WSL 2 config settings at %userprofile%/.wslconfig
std::wstring LxssWriteWslConfig(const std::wstring& Content)
{
    auto path = getenv("userprofile") + std::string("\\.wslconfig");

    std::wifstream configRead(path);
    auto previousContent = std::wstring{std::istreambuf_iterator<wchar_t>(configRead), {}};
    configRead.close();

    std::wofstream config(path);
    VERIFY_IS_TRUE(config.good());
    config << Content;

    return previousContent;
}

// writes distro specific settings /etc/wsl.conf
std::string LxssWriteWslDistroConfig(const std::string& Content)
{
    std::string path = std::format("\\\\wsl.localhost\\{}\\etc\\wsl.conf", LXSS_DISTRO_NAME_TEST);

    std::ifstream distroConfigRead(path);
    auto previousContent = std::string{std::istreambuf_iterator<char>(distroConfigRead), {}};
    distroConfigRead.close();

    std::ofstream distroConfig(path, std::ios_base::binary);
    VERIFY_IS_TRUE(distroConfig.good());
    distroConfig.write(Content.c_str(), Content.size());

    return previousContent;
}

// generates a sample global WSL config for the tests
std::wstring LxssGenerateTestConfig(TestConfigDefaults Default)
{
    WEX::Common::String kernelLogsArg;
    WEX::TestExecution::RuntimeParameters::TryGetValue(L"KernelLogs", kernelLogsArg);

    std::wstring kernelLogs;
    if (kernelLogsArg.IsEmpty())
    {
        kernelLogs = wil::GetCurrentDirectoryW().get() + std::wstring(L"\\kernelLogs.txt");
    }
    else
    {
        kernelLogs = kernelLogsArg;
    }

    auto boolOptionToString = [](LPCWSTR optionName, std::optional<bool> condition, bool defaultValue) {
        std::wstring value{optionName};
        value += L"=";
        value += condition.value_or(defaultValue) ? L"true" : L"false";
        value += L"\n";
        return value;
    };

    auto networkingModeToString = [](std::optional<wsl::core::NetworkingMode> mode) {
        if (mode.has_value())
        {
            std::wstring value = L"networkingMode=";
            value += wsl::shared::string::MultiByteToWide(wsl::core::ToString(mode.value()));
            value += L"\n";
            return value;
        }

        return std::wstring{};
    };

    auto drvFsModeToString = [](std::optional<DrvFsMode> mode) {
        std::wstring value;
        switch (mode.value_or(DrvFsMode::Plan9))
        {
        case DrvFsMode::Plan9:
            value = L"virtio9p=false";
            break;
        case DrvFsMode::Virtio9p:
            value = L"virtio9p=true";
            break;
        case DrvFsMode::VirtioFs:
            value = L"virtiofs=true";
            break;
        }

        value += L"\n";
        return value;
    };

    // TODO: Reset guiApplications to true by default once the virtio hang is solved.

    std::wstring newConfig =
        L"[wsl2]\n"
        L"crashDumpFolder=" +
        EscapePath(Default.CrashDumpFolder.value_or(g_dumpFolder + L"\\linux-crashes")) + L"\nmaxCrashDumpCount=" +
        std::to_wstring(Default.crashDumpCount) + L"\nvmIdleTimeout=" + std::to_wstring(Default.vmIdleTimeout.value_or(2000)) +
        L"\n"
        L"mountDeviceTimeout=120000\n"
        L"kernelBootTimeout=120000\n"
        L"debugConsoleLogFile=" +
        EscapePath(kernelLogs) +
        L"\n"
        L"telemetry=false\n" +
        boolOptionToString(L"safeMode", Default.safeMode, false) + boolOptionToString(L"guiApplications", Default.guiApplications, false) +
        L"earlyBootLogging=false\n" + networkingModeToString(Default.networkingMode) + drvFsModeToString(Default.drvFsMode);

    if (Default.kernel.has_value())
    {
        newConfig += L"kernel=" + EscapePath(Default.kernel.value()) + L"\n";
    }

    if (Default.kernelCommandLine.has_value())
    {
        newConfig += L"kernelCommandLine=" + Default.kernelCommandLine.value() + L"\n";
    }

    if (Default.kernelModules.has_value())
    {
        newConfig += L"kernelModules=" + EscapePath(Default.kernelModules.value()) + L"\n";
    }

    if (Default.loadKernelModules.has_value())
    {
        newConfig += L"loadKernelModules=" + Default.loadKernelModules.value() + L"\n";
    }

    if (Default.loadDefaultKernelModules.has_value())
    {
        newConfig +=
            L"loadDefaultKernelModules=" + std::wstring(Default.loadDefaultKernelModules.value() ? L"true" : L"false") + L"\n";
    }

    switch (Default.networkingMode.value_or(wsl::core::NetworkingMode::Nat))
    {
    case wsl::core::NetworkingMode::Nat:
    {
        if (Default.dnsProxy.has_value())
        {
            newConfig += boolOptionToString(L"dnsProxy", Default.dnsProxy, false);
        }

        if (Default.firewall.has_value())
        {
            newConfig += L"[experimental]\nfirewall=";
            newConfig += *Default.firewall ? L"true" : L"false";
            newConfig += L"\n[wsl2]\n";
        }

        break;
    }
    case wsl::core::NetworkingMode::Bridged:
    {
        VERIFY_IS_TRUE(Default.vmSwitch.has_value());

        newConfig += L"vmSwitch=" + *Default.vmSwitch;

        if (Default.macAddress.has_value())
        {
            newConfig += L"\nmacAddress=" + *Default.macAddress;
        }

        newConfig += L"\nipv6=" + std::wstring(Default.ipv6 ? L"true" : L"false");
        newConfig += L"\n";

        break;
    }
    }

    if (Default.dnsTunneling.has_value())
    {
        newConfig += L"\n[experimental]\n";
        newConfig += boolOptionToString(L"dnsTunneling", Default.dnsTunneling, false);
        newConfig += L"[wsl2]\n";
    }

    if (Default.dnsTunnelingIpAddress.has_value())
    {
        newConfig += L"\n[experimental]\n";
        newConfig += L"dnsTunnelingIpAddress=" + Default.dnsTunnelingIpAddress.value() + L"\n";
        newConfig += L"[wsl2]\n";
    }

    // always add this regardless if it has value, want to have it disabled by default for tests
    newConfig += L"\n[experimental]\n";
    newConfig += boolOptionToString(L"autoProxy", Default.autoProxy, false);
    newConfig += L"[wsl2]\n";

    if (Default.sparse.has_value())
    {
        std::wstring value = Default.sparse.value() ? L"true" : L"false";
        newConfig += L"[experimental]\nsparseVhd=" + value + L"\n[wsl2]";
    }

    if (Default.hostAddressLoopback.has_value())
    {
        newConfig += L"\n[experimental]\n";
        newConfig += boolOptionToString(L"hostAddressLoopback", Default.hostAddressLoopback, false);
        newConfig += L"[wsl2]\n";
    }

    // TODO: Remove once SetVersion() truncated archive error is root caused.
    newConfig += L"\n[experimental]\nSetVersionDebug=true\n[wsl2]\n";

    return newConfig;
}

std::wstring EscapePath(std::wstring_view Path)
{
    std::wstring escaped;
    for (const auto e : Path)
    {
        escaped += e;

        if (e == L'\\')
        {
            escaped += e;
        }
    }

    return escaped;
}

NTSTATUS
LxsstuParseLinuxLogFiles(__in PCWSTR LogFileName, __out PBOOL TestPassed)

/*++

Routine Description:

    Parses the output of the linux test and relogs the output.

Arguments:

    LogFileName - Supplies a string containing the log files for the test
        separated by LXSS_TEST_LOG_SEPARATOR_CHAR.

    TestPassed - Supplies a buffer to receive a boolean value specifying if the
        tests completed without errors.

Return Value:

    NTSTATUS

--*/

{

    HANDLE LinuxLogFile;
    WCHAR LinuxLogPath[MAX_PATH];
    WCHAR LocalLogFileBuffer[MAX_PATH];
    PWCHAR LogFileToken;
    DWORD PrintStatus;
    NTSTATUS Status;
    std::wstring TestDirectory;
    LXSS_TEST_LAUNCHER_TEST TestRecord;
    PWCHAR TokenState;

    LinuxLogFile = INVALID_HANDLE_VALUE;
    Status = STATUS_UNSUCCESSFUL;
    *TestPassed = FALSE;
    RtlZeroMemory(&TestRecord, sizeof(TestRecord));

    //
    // Make a copy of the log file name so wcstok can modify it.
    //

    PrintStatus = swprintf_s(LocalLogFileBuffer, RTL_NUMBER_OF(LocalLogFileBuffer), L"%s", LogFileName);

    if (PrintStatus == -1)
    {
        Status = STATUS_UNSUCCESSFUL;
        LogError("Increase LocalLogFileBuffer buffer");
        goto ErrorExit;
    }

    //
    // Get the test directory.
    //

    TestDirectory = LxsstuGetTestDirectory();

    //
    // Parse the logs for the test and determine how many passes / errors there
    // were.
    //

    LogFileToken = wcstok(LocalLogFileBuffer, LXSS_TEST_LOG_SEPARATOR_CHAR, &TokenState);

    while (LogFileToken != NULL)
    {
        LogInfo("LOGFILE: %s", LogFileToken);
        PrintStatus = swprintf_s(LinuxLogPath, RTL_NUMBER_OF(LinuxLogPath), L"%s\\log\\%s", TestDirectory.c_str(), LogFileToken);

        if (PrintStatus == -1)
        {
            Status = STATUS_UNSUCCESSFUL;
            LogError("Increase LinuxLogPath buffer");
            goto ErrorExit;
        }

        //
        // For VM Mode, copy the output file out of the ext4 volume so it can
        // be read.
        //

        if (LxsstuVmMode())
        {
            std::wstring Command = L"/bin/cp /data/test/log/";
            Command += LogFileToken;
            Command += L" $(wslpath '";
            Command += LinuxLogPath;
            Command += L"')";
            VERIFY_NO_THROW(LxsstuRunTest(Command.c_str()));
        }

        LinuxLogFile =
            CreateFileW(LinuxLogPath, GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_WRITE), NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (LinuxLogFile == INVALID_HANDLE_VALUE)
        {
            Status = STATUS_UNSUCCESSFUL;
            LogError("Could not open {:%s:} after running test, LastError %#x", LinuxLogPath, GetLastError());

            goto ErrorExit;
        }

        Status = LxsstuParseLogFile(LinuxLogFile, &TestRecord);
        if (!NT_SUCCESS(Status))
        {
            goto ErrorExit;
        }

        if (TestRecord.NumberOfErrors > 0)
        {
            LogError("LOG FILE SUMMARY: %s - PASSED: %u ERRORS: %u", LogFileToken, TestRecord.NumberOfPasses, TestRecord.NumberOfErrors);
        }
        else if (TestRecord.NumberOfPasses > 0)
        {
            LogPass("LOG FILE SUMMARY: %s - PASSED: %u ERRORS: %u", LogFileToken, TestRecord.NumberOfPasses, TestRecord.NumberOfErrors);
        }
        else
        {
            LogError("LOG FILE SUMMARY: %s - log had no passes or errors, ensure test was actually run", LogFileToken);
        }

        CloseHandle(LinuxLogFile);
        LinuxLogFile = INVALID_HANDLE_VALUE;
        LogFileToken = wcstok(NULL, LXSS_TEST_LOG_SEPARATOR_CHAR, &TokenState);
    }

    Status = STATUS_SUCCESS;

ErrorExit:
    if (LinuxLogFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(LinuxLogFile);
    }

    if ((TestRecord.NumberOfErrors == 0) && (TestRecord.NumberOfPasses > 0))
    {
        *TestPassed = TRUE;
    }

    return Status;
}

NTSTATUS
LxsstuParseLogFile(__in HANDLE FileHandle, __in PLXSS_TEST_LAUNCHER_TEST TestRecord)

/*++

Routine Description:

    Parses a single log file.

Arguments:

    TestName - Name of the test.

    LogFileName - string containing the log files for the test separated by
        LXSS_TEST_LOG_SEPARATOR_CHAR.

Return Value:

    NTSTATUS

--*/

{

    PBYTE Buffer;
    DWORD BytesRead;
    DWORD FileSize;
    DWORD FileSizeHigh;
    PCHAR Message;
    LXSS_TEST_LAUNCHER_MESSAGE_TYPE MessageType;
    NTSTATUS Status;
    PCHAR Token;

    Buffer = NULL;
    Status = STATUS_UNSUCCESSFUL;

    FileSize = GetFileSize(FileHandle, &FileSizeHigh);
    Buffer = (PBYTE)ALLOC(FileSize + 1);
    if (Buffer == NULL)
    {
        goto ErrorExit;
    }

    Buffer[FileSize] = '\0';

    do
    {
        RtlZeroMemory(Buffer, FileSize);
        if (ReadFile(FileHandle, Buffer, FileSize, &BytesRead, NULL) == FALSE)
        {

            Status = STATUS_UNSUCCESSFUL;
            LogError("ReadFile failed, LastError %#x", GetLastError());
            goto ErrorExit;
        }

        if (BytesRead == 0)
        {
            break;
        }

        //
        // Parse the log line-by-line.
        //

        Token = strtok((PCHAR)Buffer, "\n");
        while (Token != NULL)
        {

            //
            // A well-formed message begins with a timestamp and then is either
            // a start, info, error, or pass message.  For example:
            // [12:30:05.432] ERROR: Something went wrong!
            //
            // Anything that does not fit this format is re-logged an an "info"
            // message.
            //

            MessageType = LogInfoMessage;
            if (Token[0] == '[')
            {
                Message = strchr(Token, ' ');
                if ((Message == NULL) || (strlen(Message) < 2))
                {
                    break;
                }

                switch (Message[1])
                {
                case 'E':
                case 'R':
                    MessageType = LogErrorMessage;
                    break;

                case 'P':
                    MessageType = LogPassMessage;
                    break;
                }
            }

            switch (MessageType)
            {
            case LogInfoMessage:
                if (g_RelogEverything != FALSE)
                {
                    LogInfo("%S", Token);
                }

                break;

            case LogErrorMessage:
                TestRecord->NumberOfErrors += 1;
                if (g_RelogEverything != FALSE)
                {
                    LogError("%S", Token);
                }

                break;

            case LogPassMessage:
                TestRecord->NumberOfPasses += 1;
                if (g_RelogEverything != FALSE)
                {
                    LogPass("%S", Token);
                }

                break;

                DEFAULT_UNREACHABLE;
            }

            Token = strtok(NULL, "\n");
        }
    } while (BytesRead > 0);

    Status = STATUS_SUCCESS;

ErrorExit:
    if (Buffer != NULL)
    {
        FREE(Buffer);
    }

    return Status;
}

VOID LxsstuRunTest(_In_ PCWSTR CommandLine, _In_opt_ PCWSTR LogFileName, _In_opt_ PCWSTR Username) noexcept(false)

/*++

Routine Description:

    Run an individual test.

Arguments:

    CommandLine - Command line path and arguments to pass
    LogFileName - Name of the linux log file
    Username - User to run the test as, if one is not supplied the test
        will be run as root

Return Value:

    None.

--*/

{

    BOOL TestPassed;
    std::wstring LaunchArguments{};

    if (ARGUMENT_PRESENT(Username))
    {
        LaunchArguments += WSL_USER_ARG L" ";
        LaunchArguments += Username;
        LaunchArguments += L" ";
    }

    LaunchArguments += CommandLine;
    LogInfo("Test process exited with: %lu", LxsstuLaunchWsl(LaunchArguments.c_str()));

    //
    // Parse the contents of the linux log(s) files and relog.
    //

    if (ARGUMENT_PRESENT(LogFileName))
    {
        THROW_IF_NTSTATUS_FAILED(LxsstuParseLinuxLogFiles(LogFileName, &TestPassed));

        THROW_HR_IF(E_FAIL, !TestPassed);
    }

    return;
}

bool ModuleSetup(VOID)

/*++

Routine Description:

    Configures the machine to run tests

Arguments:

Return Value:

    None.

--*/

{
// Don't crash for unknown exceptions (makes debugging testpasses harder)
#ifndef _DEBUG
    wil::g_fResultFailFastUnknownExceptions = false;
#endif

    WslTraceLoggingInitialize(LxssTelemetryProvider, true);
    wsl::windows::common::EnableContextualizedErrors(false);

    auto getOptionalTestParam = [](LPCWSTR Name) -> std::optional<std::wstring> {
        WEX::Common::String Value;

        WEX::TestExecution::RuntimeParameters::TryGetValue(Name, Value);

        return Value.IsEmpty() ? std::optional<std::wstring>() : static_cast<LPCWSTR>(Value);
    };

    auto getTestParam = [&](LPCWSTR Name) -> std::wstring {
        auto value = getOptionalTestParam(Name);
        if (!value.has_value())
        {
            const std::wstring error = L"Missing TE argument: " + std::wstring(Name);
            VERIFY_FAIL(error.c_str());
        }

        return value.value();
    };

    try
    {
        const auto buildString = wsl::windows::common::registry::ReadString(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"BuildLabEx");

        LogInfo("OS build string: %ls", buildString.c_str());
    }
    CATCH_LOG();

    try
    {
        const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
        g_originalDefaultDistro = wsl::windows::common::registry::ReadString(userKey.get(), nullptr, L"DefaultDistribution", L"");
    }
    CATCH_LOG();

    g_originalConfig = LxssWriteWslConfig(LxssGenerateTestConfig());

    const auto redirectStdout = getOptionalTestParam(L"RedirectStdout");
    const auto redirectStderr = getOptionalTestParam(L"RedirectStderr");

    if (redirectStdout.has_value())
    {
        g_OriginalStdout = LxssRedirectOutput(STD_OUTPUT_HANDLE, redirectStdout.value());
    }

    if (redirectStderr.has_value())
    {
        g_OriginalStderr = LxssRedirectOutput(STD_ERROR_HANDLE, redirectStderr.value());
    }

    g_dumpFolder = getOptionalTestParam(L"DumpFolder").value_or(L".");
    g_dumpToolPath = getOptionalTestParam(L"DumpTool");
    g_pipelineBuildId = getOptionalTestParam(L"PipelineBuildId").value_or(L"");

    if (!g_pipelineBuildId.empty())
    {
        LogInfo("Pipeline build id: %ls", g_pipelineBuildId.c_str());
    }

    WEX::TestExecution::RuntimeParameters::TryGetValue(L"WerReport", g_enableWerReport);
    WEX::TestExecution::RuntimeParameters::TryGetValue(L"LogDmesg", g_LogDmesgAfterEachTest);

    g_WatchdogTimer = CreateThreadpoolTimer(LxsstuWatchdogTimer, nullptr, nullptr);
    VERIFY_IS_NOT_NULL(g_WatchdogTimer);

    ULARGE_INTEGER fileTimeConvert{};
    fileTimeConvert.QuadPart = LXSS_WATCHDOG_TIMEOUT;
    fileTimeConvert.QuadPart *= (-1 * 1000 * 10i64); // fileTime is unsigned- took out -1; check if this causes errors later
    FILETIME DueTime{};
    DueTime.dwLowDateTime = fileTimeConvert.LowPart;
    DueTime.dwHighDateTime = fileTimeConvert.HighPart;
    SetThreadpoolTimer(g_WatchdogTimer, &DueTime, 0, LXSS_WATCHDOG_TIMEOUT_WINDOW);

    const auto version = getTestParam(L"Version");
    if (version == L"1")
    {
        g_VmMode = false;
    }
    else if (version == L"2")
    {
        g_VmMode = true;
    }
    else
    {
        LogError("Unexpected version: %ls", version.c_str());
        VERIFY_FAIL();
    }

    g_testDistroPath = getTestParam(L"DistroPath");

    const auto setupScript = getOptionalTestParam(L"SetupScript");
    if (!setupScript.has_value())
    {
        // If no setup script is present, mark test_distro as the default distro here for convenience.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--set-default " LXSS_DISTRO_NAME_TEST_L), 0L);

        return true;
    }

    std::wstring Cmd =
        L"Powershell \
        -NoProfile \
        -ExecutionPolicy Bypass \
        -Command \"" +
        setupScript.value() + L" -Version '" + getTestParam(L"Version") + L"'" + L" -DistroPath " + g_testDistroPath +
        L" -DistroName " + LXSS_DISTRO_NAME_TEST_L + L" -Package '" + getTestParam(L"Package") + L"'" + L" -UnitTestsPath " +
        getOptionalTestParam(L"UnitTestsPath").value_or(L"$null");

    if (getOptionalTestParam(L"AllowUnsigned") == L"1")
    {
        Cmd += L" -AllowUnsigned";
    }

    Cmd += +L"\"";

    LogInfo("Running test setup command: %ls", Cmd.c_str());

    const auto ExitCode = LxsstuRunCommand(Cmd.data());
    if (ExitCode != 0)
    {
        THROW_HR_MSG(E_FAIL, "Test setup returned non-zero exit code %lu", ExitCode);
    }

    return true;
}

bool ModuleCleanup(VOID)

/*++

Routine Description:

    Called after the tests cases have been executed.
    Reverts WSL version upgrades, if any.

Arguments:
    None.

Return Value:

    None.

--*/

{
    LogInfo("Exiting UnitTests module");

    //
    // Release the watchdog timer.
    //

    if (g_WatchdogTimer != NULL)
    {
        SetThreadpoolTimer(g_WatchdogTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(g_WatchdogTimer, true);
        CloseThreadpoolTimer(g_WatchdogTimer);
    }

    // Save the Appx & defender logs in the dump folder
    if (!g_pipelineBuildId.empty())
    {
        auto commandLine = std::format(L"Get-AppPackageLog -All > \"{}\\appx-logs.txt\"", g_dumpFolder);
        LxsstuLaunchPowershellAndCaptureOutput(commandLine.data());

        commandLine = std::format(L"Get-MpThreatDetection > \"{}\\Get-MpThreatDetection.txt\"", g_dumpFolder);
        LxsstuLaunchPowershellAndCaptureOutput(commandLine.data());

        commandLine = std::format(L"Get-MpThreat > \"{}\\Get-MpThreat.txt\"", g_dumpFolder);
        LxsstuLaunchPowershellAndCaptureOutput(commandLine.data());

        commandLine = std::format(L"Get-MpPreference > \"{}\\Get-MpPreference.txt\"", g_dumpFolder);
        LxsstuLaunchPowershellAndCaptureOutput(commandLine.data());
    }

    if (!g_originalConfig.empty())
    {
        LogInfo("Restoring .wslconfig");
        LxssWriteWslConfig(g_originalConfig);
    }

    if (!g_originalDefaultDistro.empty())
    {
        // Edge case: If the previous default distro was the test distro, it might have been deleted during the testpass.
        // Validate the distro exists before restoring.

        const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();

        try
        {
            wsl::windows::common::registry::OpenKey(userKey.get(), g_originalDefaultDistro.c_str(), KEY_READ);
        }
        catch (...)
        {
            LogInfo("Previous default distro doesn't exist anymore: '%ls', skipping restore", g_originalDefaultDistro.c_str());
            return true;
        }

        LogInfo("Restoring default distro: '%ls", g_originalDefaultDistro.c_str());

        wsl::windows::common::registry::WriteString(userKey.get(), nullptr, L"DefaultDistribution", g_originalDefaultDistro.c_str());
    }

    WslTraceLoggingUninitialize();

    return true;
}

HANDLE
LxssRedirectOutput(_In_ DWORD Stream, _In_ const std::wstring& File)

/*++

Routine Description:

    Redirect a standard stream to a file

Arguments:
    Stream - The stream to redirect

    File - The file to redirect the output to

Return Value:

    None.

--*/

{
    const HANDLE OriginalHandle = GetStdHandle(Stream);

    SECURITY_ATTRIBUTES Attributes = {0};
    Attributes.nLength = sizeof(Attributes);
    Attributes.bInheritHandle = true;

    const auto Handle =
        CreateFileW(File.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &Attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    VERIFY_IS_NOT_NULL(Handle);

    VERIFY_IS_TRUE(SetStdHandle(Stream, Handle));

    return OriginalHandle;
}

void CreateUser(_In_ const std::wstring& Username, _Out_ PULONG Uid, _Out_ PULONG Gid)
{
    //
    // Create the user account.
    //
    // N.B. The user may already exist if the test was run previously.
    //

    std::wstring CreateUser{L"/usr/sbin/adduser --quiet --force-badname --disabled-password --gecos \"\" "};
    CreateUser += Username.c_str();
    LxsstuLaunchWsl(CreateUser.c_str());

    //
    // Create an unnamed pipe to read the output of the launched commands.
    //

    wil::unique_handle ReadPipe;
    wil::unique_handle WritePipe;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&ReadPipe, &WritePipe, NULL, 0));

    //
    // Mark the write end of the pipe as inheritable.
    //

    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(WritePipe.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

    //
    // Query the UID.
    //

    std::wstring QueryUid{L"/usr/bin/id -u "};
    QueryUid += Username.c_str();
    THROW_HR_IF(E_UNEXPECTED, (LxsstuLaunchWsl(QueryUid.c_str(), nullptr, WritePipe.get()) != 0));

    CHAR Buffer[64];
    DWORD BytesRead;
    THROW_IF_WIN32_BOOL_FALSE(ReadFile(ReadPipe.get(), Buffer, (sizeof(Buffer) - 1), &BytesRead, NULL));
    Buffer[BytesRead] = ANSI_NULL;
    const ULONG UidLocal = std::stoul(Buffer, nullptr, 10);

    //
    // Query the GID.
    //

    std::wstring QueryGid{L"/usr/bin/id -g "};
    QueryGid += Username.c_str();
    THROW_HR_IF(E_UNEXPECTED, (LxsstuLaunchWsl(QueryGid.c_str(), nullptr, WritePipe.get()) != 0));

    THROW_IF_WIN32_BOOL_FALSE(ReadFile(ReadPipe.get(), Buffer, (sizeof(Buffer) - 1), &BytesRead, NULL));
    Buffer[BytesRead] = ANSI_NULL;
    const ULONG GidLocal = std::stoul(Buffer, nullptr, 10);

    //
    // Return the queried values to the user.
    //

    *Uid = UidLocal;
    *Gid = GidLocal;
}

std::pair<HANDLE, HANDLE> UseOriginalStdHandles(VOID)

/*++

Routine Description:

    Restores the original stdout & stderr handles, if any.

Arguments:
    None.

Return Value:

    A pair of the previous stdout & stderr handles.

--*/

{
    HANDLE PreviousStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE PreviousStderr = GetStdHandle(STD_ERROR_HANDLE);

    if (g_OriginalStdout != nullptr)
    {
        VERIFY_IS_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, g_OriginalStdout));
    }

    if (g_OriginalStderr != nullptr)
    {
        VERIFY_IS_TRUE(SetStdHandle(STD_ERROR_HANDLE, g_OriginalStderr));
    }

    return {PreviousStdout, PreviousStderr};
}

void RestoreTestStdHandles(_In_ const std::pair<HANDLE, HANDLE>& handles)

/*++

Routine Description:

    Assign stdout & stderr handles.

Arguments:
    None.

Return Value:

    None.

--*/

{
    VERIFY_IS_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, handles.first));
    VERIFY_IS_TRUE(SetStdHandle(STD_ERROR_HANDLE, handles.second));
}

bool TryLoadDnsResolverMethods() noexcept
{
    constexpr auto c_dnsModuleName = L"dnsapi.dll";
    const wil::shared_hmodule dnsModule{LoadLibraryEx(c_dnsModuleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!dnsModule)
    {
        return false;
    }

    try
    {
        // attempt to find the functions for the DNS tunneling OS APIs.
        static LxssDynamicFunction<decltype(DnsQueryRaw)> dnsQueryRaw{dnsModule, "DnsQueryRaw"};
        static LxssDynamicFunction<decltype(DnsCancelQueryRaw)> dnsCancelQueryRaw{dnsModule, "DnsCancelQueryRaw"};
        static LxssDynamicFunction<decltype(DnsQueryRawResultFree)> dnsQueryRawResultFree{dnsModule, "DnsQueryRawResultFree"};

        // Make a dummy call to the DNS APIs to verify if they are working. The APIs are going to be present
        // on older OS versions, where they can be turned on/off using a KIR. If the KIR is turned off, the APIs
        // will be unusable and will return ERROR_CALL_NOT_IMPLEMENTED.
        THROW_HR_IF(E_NOTIMPL, dnsQueryRaw(nullptr, nullptr) == ERROR_CALL_NOT_IMPLEMENTED);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool AreExperimentalNetworkingFeaturesSupported()
{
    constexpr auto NETWORKING_EXPERIMENTAL_FLOOR_BUILD = 25885;
    constexpr auto GALLIUM_FLOOR_BUILD = 25846;
    const auto build = wsl::windows::common::helpers::GetWindowsVersion();
    return ((build.BuildNumber < GALLIUM_FLOOR_BUILD) || (build.BuildNumber >= GALLIUM_FLOOR_BUILD && build.BuildNumber >= NETWORKING_EXPERIMENTAL_FLOOR_BUILD));
}

bool IsHyperVFirewallSupported() noexcept
{
    try
    {
        // Query for the Hyper-V Firewall profile object. If this object is successfully queried, then
        // the OS has the necessary Hyper-V firewall support.
        LxsstuLaunchPowershellAndCaptureOutput(L"Get-NetFirewallHyperVProfile");
    }
    catch (...)
    {
        return false;
    }
    return true;
}

std::optional<GUID> GetDistributionId(LPCWSTR Name)
{
    // Get the GUID of the test distro
    wsl::windows::common::SvcComm service;
    for (const auto& e : service.EnumerateDistributions())
    {
        if (wsl::shared::string::IsEqual(e.DistroName, Name))
        {
            return e.DistroGuid;
        }
    }

    return {};
}

wil::unique_hkey OpenDistributionKey(LPCWSTR Name)
{
    const auto id = GetDistributionId(Name);
    if (!id.has_value())
    {
        return {};
    }

    const auto idString = wsl::shared::string::GuidToString<wchar_t>(id.value());

    const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
    return wsl::windows::common::registry::OpenKey(userKey.get(), idString.c_str(), KEY_ALL_ACCESS);
}

bool WslShutdown()
{
    return VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(WSL_SHUTDOWN_ARG));
}

void TerminateDistribution(LPCWSTR DistributionName)
{
    VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(std::format(L"{} {}", WSL_TERMINATE_ARG, DistributionName)));
}

void ValidateOutput(LPCWSTR CommandLine, const std::wstring& ExpectedOutput, const std::wstring& ExpectedWarnings, int ExitCode)
{
    auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(CommandLine, ExitCode);

    VERIFY_ARE_EQUAL(ExpectedOutput, output);
    VERIFY_ARE_EQUAL(ExpectedWarnings, warnings);
}

// Trim helper method

void Trim(std::wstring& string)
{
    // Remove any extra chars (lf, spaces, ...)
    std::erase_if(string, [](auto c) { return !isalnum(c); });
}

ScopedEnvVariable::ScopedEnvVariable(const std::wstring& Name, const std::wstring& Value) : m_name(Name)
{
    VERIFY_IS_TRUE(SetEnvironmentVariable(Name.c_str(), Value.c_str()));
}

ScopedEnvVariable::~ScopedEnvVariable()
{
    VERIFY_IS_TRUE(SetEnvironmentVariable(m_name.c_str(), nullptr));
}

UniqueWebServer::UniqueWebServer(LPCWSTR Endpoint, LPCWSTR Content)
{
    auto cmd = std::format(
        LR"(Powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
$ErrorActionPreference = 'Stop'
$server = New-Object System.Net.HttpListener
$server.Prefixes.Add('{}')
$server.Start()
while ($true)
{{
    $context = $server.GetContext()
    $context.Response.StatusCode
    $content = [Text.Encoding]::UTF8.GetBytes('{}')
    $context.Response.OutputStream.Write($content , 0, $content.length)
    $context.Response.close()
}}")",
        Endpoint,
        Content);

    m_process = LxsstuStartProcess(cmd.data());
}

UniqueWebServer::UniqueWebServer(LPCWSTR Endpoint, const std::filesystem::path& File)
{
    auto cmd = std::format(
        LR"(Powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
$ErrorActionPreference = 'Stop'
$server = New-Object System.Net.HttpListener
$server.Prefixes.Add('{}')
$server.Start()
while ($true)
{{
    $context = $server.GetContext()
    $context.Response.StatusCode
    $content = [System.IO.File]::ReadAllBytes('{}')
    $context.Response.ContentLength64 = $content.length
    $context.Response.ContentType = 'application/octet-stream'
    $context.Response.OutputStream.Write($content, 0, $content.length)
    $context.Response.close()
}}")",
        Endpoint,
        File.wstring());

    m_process = LxsstuStartProcess(cmd.data());
}

UniqueWebServer::~UniqueWebServer()
{
    if (!TerminateProcess(m_process.get(), 0))
    {
        LogError("TerminateProcess failed, %lu", GetLastError());
    }
}

DistroFileChange::DistroFileChange(LPCWSTR Path, bool exists) : m_path(Path)
{
    if (exists)
    {
        m_originalContent = LxsstuLaunchWslAndCaptureOutput(std::format(L"cat '{}'", m_path)).first;
    }
}

DistroFileChange::~DistroFileChange()
{
    if (m_originalContent.has_value())
    {
        SetContent(m_originalContent->c_str());
    }
    else
    {
        Delete();
    }
}

void DistroFileChange::SetContent(LPCWSTR Content)
{
    const auto cmd = LxssGenerateWslCommandLine(std::format(L" -u root cat > '{}'", m_path).c_str());
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());

    auto [read, write] = CreateSubprocessPipe(true, false);

    process.SetStdHandles(read.get(), nullptr, nullptr);
    const auto processHandle = process.Start();

    const auto utf8content = wsl::shared::string::WideToMultiByte(Content);
    auto index = 0;

    while (index < utf8content.size())
    {
        DWORD written{};

        VERIFY_IS_TRUE(WriteFile(write.get(), utf8content.data() + index, static_cast<DWORD>(utf8content.size() - index), &written, nullptr));

        index += written;
    }

    write.reset();

    VERIFY_ARE_EQUAL(wsl::windows::common::SubProcess::GetExitCode(processHandle.get()), 0L);
}

void DistroFileChange::Delete()
{
    VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"-u root rm -f '{}'", m_path).c_str()), 0L);
}
