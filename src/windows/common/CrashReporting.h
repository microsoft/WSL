// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include "ElfCore.h"

namespace wsl::windows::common::crashreporting {

// The number of parameters registered for the WSLCProcessCrash event. Watson numbers these P0-P8
// and rejects any report whose parameter count does not match the registered signature.
constexpr size_t c_parameterCount = 9;

// Reported for any parameter whose value cannot be determined. Watson treats an empty parameter
// as a different signature, so every parameter always carries a value.
constexpr auto c_unknownValue = L"<unknown>";

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

    // The image name of the Windows process that created the session the crash happened in, so
    // reports can be attributed to the application that asked for the container. Empty when there
    // is no such process.
    std::wstring CallingProcess;
};

// Returns the executable's base name, accepting either the '!' encoded form used by core_pattern
// or a conventional path. Returns c_unknownValue when there is no name.
std::wstring GetExecutableName(_In_ const std::string& Executable);

// Builds the WSLCProcessCrash parameter set. Separated from submission so the mapping can be
// tested without talking to Windows Error Reporting.
//
// Core holds the details read from the core. Without it the report names the crashing executable
// from Info and reports every detail that only the core holds as c_unknownValue.
ReportParameters BuildReportParameters(_In_ const LinuxCrashInfo& Info, _In_ const std::optional<elfcore::CoreDetails>& Core = std::nullopt);

// Submits a WSLCProcessCrash report with the guest's ELF core attached as report data.
HRESULT ReportLinuxCrash(_In_ const LinuxCrashInfo& Info) noexcept;

// Whether crash reports should be submitted. Reporting uploads the crashing process's full ELF
// core, so it is opt-in.
bool IsReportingEnabled() noexcept;

} // namespace wsl::windows::common::crashreporting
