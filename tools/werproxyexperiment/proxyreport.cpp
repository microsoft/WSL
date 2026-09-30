// Copyright (C) Microsoft Corporation. All rights reserved.
//
// Process B of the WER proxy reporting experiment.
//
// Submits a Windows Error Reporting report describing a *different* process: the report's
// WER_REPORT_INFORMATION.hProcess and the collected dump both refer to the target, not to this
// process. That is the piece being validated, along with whether WER stops the target while it
// works.
//
// Every WER call is bracketed by QueryPerformanceCounter, and the raw counter values are printed.
// QPC is a system wide timebase, so those values line up with the interruptions that the target
// (spintarget.cpp, running as winget.exe) reports, and a pause can be attributed to a specific call.

#include <windows.h>
#include <werapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr DWORD c_maxParameters = 10;

// The flags the WSL test harness submits crash reports with, see test/windows/Common.cpp.
constexpr DWORD c_defaultSubmitFlags = WER_SUBMIT_ADD_REGISTERED_DATA | WER_SUBMIT_NO_CLOSE_UI |
                                       WER_SUBMIT_BYPASS_DATA_THROTTLING | WER_SUBMIT_REPORT_MACHINE_ID | WER_SUBMIT_QUEUE;

long long g_frequency = 0;
long long g_start = 0;

long long Now()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double ToMilliseconds(long long Ticks)
{
    return (static_cast<double>(Ticks) * 1000.0) / static_cast<double>(g_frequency);
}

// Prints the QPC window a call occupied, so it can be matched against the target's gap report.
long long Trace(const wchar_t* Step, long long Started)
{
    const auto now = Now();
    wprintf(L"  [t+%8.2f ms] %-22ls took %8.2f ms  (qpc %lld..%lld)\n", ToMilliseconds(Started - g_start), Step, ToMilliseconds(now - Started), Started, now);
    fflush(stdout);
    return now;
}

struct HandleDeleter
{
    void operator()(HANDLE Value) const
    {
        if (Value != nullptr && Value != INVALID_HANDLE_VALUE)
        {
            CloseHandle(Value);
        }
    }
};

using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

const wchar_t* SubmitResultName(WER_SUBMIT_RESULT Result)
{
    switch (Result)
    {
    case WerReportQueued:
        return L"WerReportQueued (on disk, nothing uploaded yet)";
    case WerReportUploaded:
        return L"WerReportUploaded (event reached Watson, no cab)";
    case WerReportDebug:
        return L"WerReportDebug";
    case WerReportFailed:
        return L"WerReportFailed";
    case WerReportCancelled:
        return L"WerReportCancelled";
    case WerReportAsync:
        return L"WerReportAsync";
    case WerReportUploadedCab:
        return L"WerReportUploadedCab (cab uploaded, dump is retrievable)";
    default:
        return L"unknown";
    }
}

std::wstring ImageName(const std::wstring& Path)
{
    const auto separator = Path.find_last_of(L"\\/");
    return (separator == std::wstring::npos) ? Path : Path.substr(separator + 1);
}

std::optional<std::wstring> ImagePath(HANDLE Process)
{
    wchar_t buffer[MAX_PATH]{};
    DWORD size = ARRAYSIZE(buffer);
    if (!QueryFullProcessImageNameW(Process, 0, buffer, &size))
    {
        return std::nullopt;
    }

    return std::wstring{buffer, size};
}

// The address an AzureWatson bucket resolves against has to land in a module of the reported
// process, so the faulting address is pointed at the target's own image.
std::optional<void*> ModuleBase(HANDLE Process, const std::wstring& Module)
{
    std::vector<HMODULE> modules;
    DWORD required = 0;
    BOOL result = FALSE;
    do
    {
        modules.resize(required / sizeof(HMODULE));
        result = EnumProcessModulesEx(Process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &required, LIST_MODULES_ALL);
    } while (result && (required / sizeof(HMODULE)) > modules.size());

    if (!result)
    {
        return std::nullopt;
    }

    for (const auto& module : modules)
    {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameExW(Process, module, path, ARRAYSIZE(path)) == 0)
        {
            continue;
        }

        if (_wcsicmp(ImageName(path).c_str(), Module.c_str()) == 0)
        {
            MODULEINFO info{};
            if (GetModuleInformation(Process, module, &info, sizeof(info)))
            {
                return info.lpBaseOfDll;
            }
        }
    }

    return std::nullopt;
}

std::optional<DWORD> FirstThread(DWORD Pid)
{
    const UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};
    if (snapshot.get() == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot.get(), &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == Pid)
            {
                return entry.th32ThreadID;
            }
        } while (Thread32Next(snapshot.get(), &entry));
    }

    return std::nullopt;
}

void SetField(wchar_t* Field, size_t Size, const std::wstring& Value)
{
    wcsncpy_s(Field, Size, Value.c_str(), _TRUNCATE);
}

void PrintUsage()
{
    wprintf(
        L"Usage: proxyreport.exe --pid <pid> [options]\n"
        L"\n"
        L"  --pid <pid>              Process to report against. Required.\n"
        L"  --event <name>           WER event name (default WSLAppCrash).\n"
        L"  --dump none|mini|triage|heap\n"
        L"                           Dump to collect from the target (default mini). 'none'\n"
        L"                           isolates whether the pause comes from dump collection.\n"
        L"  --add-file <path>        Attach an existing file to the report. Unlike --dump this\n"
        L"                           reads nothing out of the target process.\n"
        L"  --param <n> <name> <value>\n"
        L"                           Override bucketing parameter n (0-9). Repeatable.\n"
        L"  --no-params              Send no bucketing parameters, letting the server bucket the\n"
        L"                           report on its own.\n"
        L"  --flags <hex>            Submit flags (default %08x).\n"
        L"  --no-submit              Build the report and stop before submitting.\n",
        c_defaultSubmitFlags);
}

} // namespace

int wmain(int Argc, wchar_t** Argv)
{
    DWORD pid = 0;
    std::wstring eventName = L"WSLAppCrash";
    std::wstring dumpKind = L"mini";
    std::wstring addFile;
    DWORD submitFlags = c_defaultSubmitFlags;
    bool submit = true;

    // The parameter schema registered for WSLAppCrash. Watson numbers these P1-P7; WerReportSetParameter
    // indexes the same slots from zero.
    std::array<std::pair<std::wstring, std::wstring>, c_maxParameters> parameters{
        {{L"AppName", L""},
         {L"AppVersion", L"1.26.0.0"},
         {L"ModuleName", L""},
         {L"ModuleVersion", L"1.26.0.0"},
         {L"ModuleOffset", L"0000000000001234"},
         {L"ExceptionCode", L"c0000005"},
         {L"Flags", L"0"},
         {L"", L""},
         {L"", L""},
         {L"", L""}}};

    for (int i = 1; i < Argc; i += 1)
    {
        const std::wstring argument{Argv[i]};
        auto value = [&]() -> const wchar_t* { return (i + 1 < Argc) ? Argv[++i] : L""; };

        if (argument == L"--pid")
        {
            pid = static_cast<DWORD>(_wtoi(value()));
        }
        else if (argument == L"--event")
        {
            eventName = value();
        }
        else if (argument == L"--dump")
        {
            dumpKind = value();
        }
        else if (argument == L"--add-file")
        {
            addFile = value();
        }
        else if (argument == L"--flags")
        {
            submitFlags = static_cast<DWORD>(wcstoul(value(), nullptr, 16));
        }
        else if (argument == L"--no-submit")
        {
            submit = false;
        }
        else if (argument == L"--no-params")
        {
            parameters = {};
        }
        else if (argument == L"--param" && (i + 3) < Argc)
        {
            const auto index = static_cast<size_t>(_wtoi(Argv[i + 1]));
            if (index >= c_maxParameters)
            {
                wprintf(L"Parameter index must be 0-%u.\n", c_maxParameters - 1);
                return 1;
            }

            parameters[index] = {Argv[i + 2], Argv[i + 3]};
            i += 3;
        }
        else
        {
            PrintUsage();
            return (argument == L"--help") ? 0 : 1;
        }
    }

    if (pid == 0)
    {
        PrintUsage();
        return 1;
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    g_start = Now();

    const UniqueHandle target{OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid)};
    if (!target)
    {
        wprintf(L"OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }

    const auto path = ImagePath(target.get());
    if (!path.has_value())
    {
        wprintf(L"Failed to read the image path of pid %lu: %lu\n", pid, GetLastError());
        return 1;
    }

    const auto name = ImageName(path.value());

    // The two name parameters default to whatever the target actually is, so that pointing this at
    // a binary named winget.exe is enough to exercise routing for the owner of that name.
    for (auto& parameter : parameters)
    {
        if (parameter.second.empty() && !parameter.first.empty())
        {
            parameter.second = name;
        }
    }

    wprintf(L"Process B\n");
    wprintf(L"  target pid    : %lu\n", pid);
    wprintf(L"  target image  : %ls\n", path.value().c_str());
    wprintf(L"  event         : %ls\n", eventName.c_str());
    wprintf(L"  dump          : %ls\n", dumpKind.c_str());
    wprintf(L"  submit flags  : 0x%08x\n", submitFlags);
    wprintf(L"  qpc at start  : %lld\n\n", g_start);

    WER_REPORT_INFORMATION information{};
    information.dwSize = sizeof(information);

    // The report describes the target, not this process. This is the proxy reporting part.
    information.hProcess = target.get();
    SetField(information.wzApplicationName, ARRAYSIZE(information.wzApplicationName), name);
    SetField(information.wzApplicationPath, ARRAYSIZE(information.wzApplicationPath), path.value());
    SetField(information.wzFriendlyEventName, ARRAYSIZE(information.wzFriendlyEventName), eventName);
    SetField(information.wzDescription, ARRAYSIZE(information.wzDescription), L"Synthetic proxy report, not a real crash");

    auto step = Now();
    HREPORT report{};
    auto result = WerReportCreate(eventName.c_str(), WerReportApplicationCrash, &information, &report);
    step = Trace(L"WerReportCreate", step);
    if (FAILED(result))
    {
        wprintf(L"WerReportCreate failed: 0x%08x\n", result);
        return 1;
    }

    for (DWORD i = 0; i < c_maxParameters; i += 1)
    {
        if (parameters[i].first.empty())
        {
            continue;
        }

        result = WerReportSetParameter(report, i, parameters[i].first.c_str(), parameters[i].second.c_str());
        if (FAILED(result))
        {
            wprintf(L"  WerReportSetParameter(P%u) failed: 0x%08x\n", i, result);
        }
        else
        {
            wprintf(L"  P%u %-18ls = %ls\n", i, parameters[i].first.c_str(), parameters[i].second.c_str());
        }
    }

    step = Trace(L"WerReportSetParameter", step);

    // Attaching a file is how an ELF core from inside the utility VM reaches a report: the file
    // already exists on disk, so WER never has to inspect the target's memory to collect it.
    if (!addFile.empty())
    {
        step = Now();
        result = WerReportAddFile(report, addFile.c_str(), WerFileTypeOther, WER_FILE_ANONYMOUS_DATA);
        step = Trace(L"WerReportAddFile", step);
        if (FAILED(result))
        {
            wprintf(L"WerReportAddFile failed: 0x%08x\n", result);
        }
        else
        {
            wprintf(L"  attached file : %ls\n", addFile.c_str());
        }
    }

    if (dumpKind != L"none")
    {
        const WER_DUMP_TYPE dumpType = (dumpKind == L"triage") ? WerDumpTypeTriageDump
                                       : (dumpKind == L"heap") ? WerDumpTypeHeapDump
                                                               : WerDumpTypeMiniDump;

        UniqueHandle thread;
        EXCEPTION_RECORD record{};
        EXCEPTION_POINTERS pointers{};
        WER_EXCEPTION_INFORMATION exception{};
        WER_EXCEPTION_INFORMATION* exceptionParameter = nullptr;

        const auto base = ModuleBase(target.get(), name);
        const auto threadId = FirstThread(pid);
        if (base.has_value() && threadId.has_value())
        {
            thread.reset(OpenThread(THREAD_ALL_ACCESS, FALSE, threadId.value()));
        }

        if (thread)
        {
            record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            record.ExceptionAddress = base.value();
            pointers.ExceptionRecord = &record;

            exception.pExceptionPointers = &pointers;

            // The pointers above are in this process, not in the target.
            exception.bClientPointers = FALSE;
            exceptionParameter = &exception;

            wprintf(L"  faulting address : %p (base of %ls in the target)\n", base.value(), name.c_str());
            wprintf(L"  faulting thread  : %lu\n", threadId.value());
        }
        else
        {
            wprintf(L"  no faulting thread or module resolved, collecting the dump without exception info\n");
        }

        step = Now();
        result = WerReportAddDump(report, target.get(), thread.get(), dumpType, exceptionParameter, nullptr, 0);
        step = Trace(L"WerReportAddDump", step);
        if (FAILED(result))
        {
            wprintf(L"WerReportAddDump failed: 0x%08x\n", result);
        }
    }

    if (!submit)
    {
        wprintf(L"\n--no-submit, stopping before WerReportSubmit.\n");
        WerReportCloseHandle(report);
        return 0;
    }

    WER_SUBMIT_RESULT submitResult{};
    step = Now();
    result = WerReportSubmit(report, WerConsentApproved, submitFlags, &submitResult);
    Trace(L"WerReportSubmit", step);

    WerReportCloseHandle(report);

    wprintf(L"\n  hr            : 0x%08x\n", result);
    wprintf(L"  submit result : %d, %ls\n", static_cast<int>(submitResult), SubmitResultName(submitResult));

    return FAILED(result) ? 1 : 0;
}
