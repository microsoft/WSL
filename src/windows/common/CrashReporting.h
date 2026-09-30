// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace wsl::windows::common::crashreporting {

// The number of parameters registered for the WSLAppCrash event. Watson numbers these P1-P7 and
// rejects any report whose parameter count does not match the registered signature.
constexpr size_t c_parameterCount = 7;

using ReportParameters = std::array<std::pair<std::wstring, std::wstring>, c_parameterCount>;

struct LinuxCrashInfo
{
    // The crashing executable as reported by the guest kernel's core_pattern %E specifier, which
    // encodes path separators as '!' (for instance "!usr!bin!bash").
    std::string Executable;
    ULONG Pid{};
    ULONG Signal{};
    ULONGLONG Timestamp{};
    std::filesystem::path CorePath;

    // The Windows process the crash is attributed to, used as AppName so reports bucket against
    // the application that asked for the container. Empty when there is no such process, in which
    // case the crashing executable is used instead.
    std::wstring AppName;
};

// Returns the executable's base name, accepting either the '!' encoded form used by core_pattern
// or a conventional path.
std::wstring GetExecutableName(_In_ const std::string& Executable);

// Builds the WSLAppCrash parameter set. Separated from submission so the mapping can be tested
// without talking to Windows Error Reporting.
ReportParameters BuildReportParameters(_In_ const LinuxCrashInfo& Info);

// Submits a WSLAppCrash report with the guest's ELF core attached as report data.
HRESULT ReportLinuxCrash(_In_ const LinuxCrashInfo& Info) noexcept;

// Whether crash reports should be submitted. Reporting uploads the crashing process's full ELF
// core, so it is opt-in.
bool IsReportingEnabled() noexcept;

} // namespace wsl::windows::common::crashreporting
