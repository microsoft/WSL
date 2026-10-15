// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include <werapi.h>
#include "CrashReporting.h"
#include "registry.hpp"
#include "WslTelemetry.h"

using wsl::windows::common::crashreporting::c_unknownValue;
using wsl::windows::common::crashreporting::LinuxCrashInfo;
using wsl::windows::common::crashreporting::ReportParameters;

namespace {

constexpr auto c_eventName = L"WSLCProcessCrash";

// Opt-in switch. wslcsession does not parse .wslconfig, so the container path is gated on the
// machine-wide WSL registry key instead.
constexpr auto c_reportingValueName = L"CrashDumpReporting";

// Matches the flags the WSL test harness submits reports with, see test/windows/Common.cpp.
// WER_SUBMIT_QUEUE keeps submission non-blocking: the report is written to the local queue and
// uploaded later, so crash collection never waits on the network.
constexpr DWORD c_submitFlags = WER_SUBMIT_ADD_REGISTERED_DATA | WER_SUBMIT_NO_CLOSE_UI | WER_SUBMIT_BYPASS_DATA_THROTTLING |
                                WER_SUBMIT_REPORT_MACHINE_ID | WER_SUBMIT_QUEUE;

// Maximum lengths registered for the event's parameters.
constexpr size_t c_maxParameterLength = 128;
constexpr size_t c_maxOffsetLength = 32;
constexpr size_t c_maxCallingProcessLength = 255;

std::wstring Limit(std::wstring Value, size_t MaxLength)
{
    if (Value.size() > MaxLength)
    {
        Value.resize(MaxLength);
    }

    return Value;
}

std::wstring ValueOrUnknown(const std::string& Value)
{
    return Value.empty() ? std::wstring{c_unknownValue} : wsl::shared::string::MultiByteToWide(Value);
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

    return name.empty() ? std::wstring{c_unknownValue} : wsl::shared::string::MultiByteToWide(name);
}

ReportParameters wsl::windows::common::crashreporting::BuildReportParameters(
    _In_ const LinuxCrashInfo& Info, _In_ const std::optional<elfcore::CoreDetails>& Core)
{
    std::wstring process = GetExecutableName(Info.Executable);
    std::wstring processBuildId = c_unknownValue;
    std::wstring module = c_unknownValue;
    std::wstring moduleBuildId = c_unknownValue;
    std::wstring offset = c_unknownValue;
    std::wstring signal = std::to_wstring(Info.Signal);
    std::wstring signalCode = c_unknownValue;
    std::wstring architecture = c_unknownValue;

    if (Core.has_value())
    {
        if (!Core->ExecutablePath.empty())
        {
            process = GetExecutableName(Core->ExecutablePath);
        }

        processBuildId = ValueOrUnknown(Core->ExecutableBuildId);
        architecture = ValueOrUnknown(Core->Architecture);

        // A program counter outside any file mapping has no module to be relative to, so the
        // absolute address is reported against an unknown module.
        if (!Core->ModulePath.empty())
        {
            module = GetExecutableName(Core->ModulePath);
            moduleBuildId = ValueOrUnknown(Core->ModuleBuildId);
        }

        offset = std::format(L"0x{:x}", Core->ModulePath.empty() ? Core->ProgramCounter : Core->ModuleOffset);

        if (Core->Signal != 0)
        {
            signal = std::to_wstring(Core->Signal);
        }

        if (Core->SignalCode.has_value())
        {
            signalCode = std::to_wstring(Core->SignalCode.value());
        }
    }

    const auto callingProcess = Info.CallingProcess.empty() ? std::wstring{c_unknownValue} : Info.CallingProcess;

    return ReportParameters{
        {{L"ProcessName", Limit(std::move(process), c_maxParameterLength)},
         {L"ProcessBuildId", Limit(std::move(processBuildId), c_maxParameterLength)},
         {L"ModuleName", Limit(std::move(module), c_maxParameterLength)},
         {L"ModuleBuildId", Limit(std::move(moduleBuildId), c_maxParameterLength)},
         {L"ModuleOffset", Limit(std::move(offset), c_maxOffsetLength)},
         {L"SignalNumber", std::move(signal)},
         {L"SICode", std::move(signalCode)},
         {L"Architecture", std::move(architecture)},
         {L"CallingProcess", Limit(callingProcess, c_maxCallingProcessLength)}}};
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
    // A core that cannot be parsed still gets reported, bucketed per executable instead of per
    // faulting frame.
    std::optional<elfcore::CoreDetails> core;
    try
    {
        core = elfcore::ReadCoreDetails(Info.CorePath);
    }
    CATCH_LOG()

    const auto parameters = BuildReportParameters(Info, core);
    const auto& process = parameters[0].second;
    const auto& signal = parameters[5].second;

    // The report is shown under the Windows application the crash is attributed to when there is
    // one, otherwise under the crashing Linux process.
    const auto& app = Info.CallingProcess.empty() ? process : parameters[8].second;

    WER_REPORT_INFORMATION information{};
    information.dwSize = sizeof(information);

    // The report is filed against this process rather than the calling application.
    // Windows Error Reporting suspends hProcess while it collects data, and the crashing process
    // lives in the utility VM, so pointing it at the calling application would stall a process
    // that did not crash without adding anything to the report.
    information.hProcess = GetCurrentProcess();
    SetField(information.wzFriendlyEventName, ARRAYSIZE(information.wzFriendlyEventName), L"Linux application crash");
    SetField(information.wzApplicationName, ARRAYSIZE(information.wzApplicationName), app);
    SetField(information.wzDescription, ARRAYSIZE(information.wzDescription), std::format(L"{} terminated by signal {}", process, signal));

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
        TraceLoggingValue(process.c_str(), "ProcessName"),
        TraceLoggingValue(parameters[8].second.c_str(), "CallingProcess"),
        TraceLoggingValue(Info.Pid, "Pid"),
        TraceLoggingValue(signal.c_str(), "Signal"),
        TraceLoggingValue(parameters[6].second.c_str(), "SICode"),
        TraceLoggingValue(parameters[2].second.c_str(), "ModuleName"),
        TraceLoggingValue(parameters[4].second.c_str(), "ModuleOffset"),
        TraceLoggingValue(static_cast<int>(submitResult), "SubmitResult"));

    return S_OK;
}
CATCH_RETURN()
