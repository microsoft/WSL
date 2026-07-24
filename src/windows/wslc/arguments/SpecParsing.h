/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SpecParsing.h

Abstract:

    Declarations for parsers that turn delimited command-line spec strings
    (e.g. --secret, --ulimit, --label, --filter) into structured values.

--*/
#pragma once

#include "ContainerModel.h"
#include "InspectModel.h"
#include <string>
#include <tuple>
#include <utility>
#include <wslc.h>

namespace wsl::windows::wslc::services {
struct BuildSecret;
struct BuildOutput;
} // namespace wsl::windows::wslc::services

namespace wsl::windows::wslc::validation {

// The two halves of a spec token split at its first separator (default '=').
// HadSeparator distinguishes "key" (no separator) from "key=" (empty value).
struct KeyValueSplit
{
    std::wstring Key;
    std::wstring Value;
    bool HadSeparator;
};

// Splits value at the first occurrence of separator. When no separator is present the whole
// string is returned as Key, Value is empty, and HadSeparator is false.
KeyValueSplit SplitKeyValue(const std::wstring& value, wchar_t separator = L'=');

// Parses a docker-style --secret spec ("id=...,type=...,src=...") and resolves its value bytes.
services::BuildSecret ParseSecretSpec(const std::wstring& spec);

// Parses a docker-style --output spec ("type=...,dest=...,<attr>=...") into a BuildOutput.
services::BuildOutput ParseOutputSpec(const std::wstring& spec);

// Serializes a BuildOutput back into a canonical buildx --output spec ("type=...,dest=...,<attr>=...").
std::wstring FormatOutputSpec(const services::BuildOutput& output);

// Parses a --ulimit spec ("<name>=<soft>[:<hard>]") into (name, soft, hard). -1 means unlimited.
std::tuple<std::string, int64_t, int64_t> ParseUlimit(const std::wstring& input, const std::wstring& argName = {});

// Parses a --label spec ("key[=value]"); the key must be non-empty.
std::pair<std::string, std::string> ParseLabel(const std::wstring& value);

// Parses a driver option spec ("key[=value]"); a missing value yields an empty string.
std::pair<std::string, std::string> ParseDriverOption(const std::wstring& value);

// Parses a --filter spec ("key=value"); the separator is required.
std::pair<std::string, std::string> ParseFilter(const std::wstring& value);

// Parses a signal by name ("SIGKILL"/"KILL", case-insensitive) or number ("9") into a WSLCSignal.
WSLCSignal GetWSLCSignalFromString(const std::wstring& input, const std::wstring& argName = {});

// Parses a timestamp given as Unix epoch seconds or an RFC3339 string into epoch seconds.
ULONGLONG GetTimestampFromString(const std::wstring& value, const std::wstring& argName = {});

// Parses an output format ("json"/"table") into a FormatType.
models::FormatType GetFormatTypeFromString(const std::wstring& input, const std::wstring& argName = {});

// Parses an inspect target ("image"/"container"/"network"/"volume") into an InspectType.
models::InspectType GetInspectTypeFromString(const std::wstring& input, const std::wstring& argName);

// Parses a memory size (e.g. "512m", "1g") into a byte count.
int64_t GetMemorySizeFromString(const std::wstring& input, const std::wstring& argName = {});

// Parses a Go-style duration (e.g. "1.5h", "500ms") into nanoseconds.
int64_t GetDurationNanosFromString(const std::wstring& input, const std::wstring& argName = {});

// Parses a fractional CPU count into nano-CPUs (cpus * 1e9).
int64_t GetNanoCpusFromString(const std::wstring& input, const std::wstring& argName = {});

} // namespace wsl::windows::wslc::validation
