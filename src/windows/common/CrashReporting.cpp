// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include <werapi.h>
#include "CrashReporting.h"
#include "registry.hpp"
#include "WslTelemetry.h"
#include "wslutil.h"

using wsl::windows::common::crashreporting::LinuxCrashInfo;
using wsl::windows::common::crashreporting::ReportParameters;

namespace {

constexpr auto c_eventName = L"WSLAppCrash";

// Opt-in switch. wslcsession does not parse .wslconfig, so the container path is gated on the
// machine-wide WSL registry key instead.
constexpr auto c_reportingValueName = L"CrashDumpReporting";

// Matches the flags the WSL test harness submits reports with, see test/windows/Common.cpp.
// WER_SUBMIT_QUEUE keeps submission non-blocking: the report is written to the local queue and
// uploaded later, so crash collection never waits on the network.
constexpr DWORD c_submitFlags = WER_SUBMIT_ADD_REGISTERED_DATA | WER_SUBMIT_NO_CLOSE_UI | WER_SUBMIT_BYPASS_DATA_THROTTLING |
                                WER_SUBMIT_REPORT_MACHINE_ID | WER_SUBMIT_QUEUE;

// The guest kernel has no notion of a Windows module version, so the installed WSL version is
// reported instead. It identifies the build whose guest components produced the core.
const std::wstring& WslVersion()
{
    static const std::wstring version = []() {
        try
        {
            const auto installed = wsl::windows::common::wslutil::GetInstalledPackageVersion();
            if (installed.has_value())
            {
                const auto& [major, minor, revision] = installed.value();
                return std::format(L"{}.{}.{}", major, minor, revision);
            }
        }
        CATCH_LOG()

        return std::wstring{L"0.0.0"};
    }();

    return version;
}

void SetField(_Out_writes_(Size) PWSTR Field, size_t Size, _In_ const std::wstring& Value)
{
    wcsncpy_s(Field, Size, Value.c_str(), _TRUNCATE);
}

} // namespace

std::wstring wsl::windows::common::crashreporting::GetExecutableName(_In_ const std::string& Executable)
{
    const auto separator = Executable.find_last_of("!/");
    const auto name = (separator == std::string::npos) ? Executable : Executable.substr(separator + 1);

    return name.empty() ? L"unknown" : wsl::shared::string::MultiByteToWide(name);
}

ReportParameters wsl::windows::common::crashreporting::BuildReportParameters(_In_ const LinuxCrashInfo& Info)
{
    const auto module = GetExecutableName(Info.Executable);
    const auto& version = WslVersion();

    // AppName identifies who the crash belongs to and ModuleName what actually faulted. For a
    // container crash those differ: the report buckets against the Windows application that
    // created the container, while the module names the Linux binary that died.
    const auto app = Info.AppName.empty() ? module : Info.AppName;

    // The module offset is not resolved yet: doing so means parsing the core's NT_FILE mappings
    // and NT_PRSTATUS registers to map the faulting address back to a shared object. Until then
    // reports bucket per executable and signal rather than per faulting frame.
    return ReportParameters{
        {{L"AppName", app},
         {L"AppVersion", version},
         {L"ModuleName", module},
         {L"ModuleVersion", version},
         {L"ModuleOffset", L"0000000000000000"},
         {L"ExceptionCode", std::format(L"{:08x}", Info.Signal)},
         {L"Flags", L"0"}}};
}

bool wsl::windows::common::crashreporting::IsReportingEnabled() noexcept
try
{
    const auto key = wsl::windows::common::registry::OpenLxssMachineKey(KEY_READ);

    return wsl::windows::common::registry::ReadDword(key.get(), nullptr, c_reportingValueName, 0) != 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

HRESULT wsl::windows::common::crashreporting::ReportLinuxCrash(_In_ const LinuxCrashInfo& Info) noexcept
try
{
    const auto parameters = BuildReportParameters(Info);
    const auto module = GetExecutableName(Info.Executable);
    const auto& app = parameters[0].second;

    WER_REPORT_INFORMATION information{};
    information.dwSize = sizeof(information);

    // The report is filed against this process rather than the application named in AppName.
    // Windows Error Reporting suspends hProcess while it collects data, and the crashing process
    // lives in the utility VM, so pointing it at the calling application would stall a process
    // that did not crash without adding anything to the report.
    information.hProcess = GetCurrentProcess();
    SetField(information.wzFriendlyEventName, ARRAYSIZE(information.wzFriendlyEventName), L"Linux application crash");
    SetField(information.wzApplicationName, ARRAYSIZE(information.wzApplicationName), app);
    SetField(information.wzDescription, ARRAYSIZE(information.wzDescription), std::format(L"{} terminated by signal {}", module, Info.Signal));

    wil::unique_any<HREPORT, decltype(&WerReportCloseHandle), WerReportCloseHandle> report;
    RETURN_IF_FAILED(WerReportCreate(c_eventName, WerReportApplicationCrash, &information, &report));

    for (size_t index = 0; index < parameters.size(); index++)
    {
        RETURN_IF_FAILED(WerReportSetParameter(
            report.get(), static_cast<DWORD>(index), parameters[index].first.c_str(), parameters[index].second.c_str()));
    }

    // The core is an ELF core written by the guest kernel, not a Windows minidump, so it is
    // attached as report data rather than via WerReportAddDump.
    RETURN_IF_FAILED(WerReportAddFile(report.get(), Info.CorePath.c_str(), WerFileTypeOther, WER_FILE_ANONYMOUS_DATA));

    WER_SUBMIT_RESULT submitResult{};
    RETURN_IF_FAILED(WerReportSubmit(report.get(), WerConsentApproved, c_submitFlags, &submitResult));

    WSL_LOG(
        "LinuxCrashReported",
        TraceLoggingValue(app.c_str(), "AppName"),
        TraceLoggingValue(module.c_str(), "Executable"),
        TraceLoggingValue(Info.Pid, "Pid"),
        TraceLoggingValue(Info.Signal, "Signal"),
        TraceLoggingValue(static_cast<int>(submitResult), "SubmitResult"));

    return S_OK;
}
CATCH_RETURN()
