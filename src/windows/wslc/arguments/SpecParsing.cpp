/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SpecParsing.cpp

Abstract:

    Parsers that turn delimited command-line spec strings (e.g. --secret,
    --ulimit, --label, --filter) into structured values. These share the
    SplitKeyValue helper for consistent key/value splitting.

--*/

#include "precomp.h"
#include "SpecParsing.h"
#include "ArgumentValidation.h"
#include "Exceptions.h"
#include "ImageService.h"
#include "JsonUtils.h"
#include "Localization.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <wslc.h>

using namespace wsl::windows::common;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc::validation {

KeyValueSplit SplitKeyValue(const std::wstring& value, wchar_t separator)
{
    const auto pos = value.find(separator);
    if (pos == std::wstring::npos)
    {
        return {value, std::wstring{}, false};
    }

    return {value.substr(0, pos), value.substr(pos + 1), true};
}

services::BuildSecret ParseSecretSpec(const std::wstring& spec)
{
    std::wstring id;
    std::wstring type;
    std::wstring envName;
    std::wstring srcPath;

    // Docker parity: buildx parses --secret as a single CSV record (go-csvvalue), so a quoted field
    // may contain commas (e.g. a 'src=' path). Malformed quoting is rejected like any other bad spec.
    const auto parts = SplitCsvFields(spec);
    if (!parts.has_value())
    {
        throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, L"malformed quoting"));
    }

    for (const auto& part : *parts)
    {
        const auto kv = SplitKeyValue(part);
        if (!kv.HadSeparator || kv.Key.empty())
        {
            throw ArgumentException(
                Localization::MessageWslcSecretInvalidSpec(spec, L"expected key=value pairs separated by ','"));
        }
        const auto& key = kv.Key;
        const auto& value = kv.Value;

        if (key == L"id")
        {
            id = value;
        }
        else if (key == L"type")
        {
            type = value;
        }
        else if (key == L"env")
        {
            envName = value;
        }
        else if (key == L"src" || key == L"source")
        {
            srcPath = value;
        }
        else
        {
            throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unsupported key '{}'", key)));
        }
    }

    if (id.empty())
    {
        throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, L"'id=' is required"));
    }

    // Docker parity: 'id' may not start with '-' because that would be interpreted as a command-line option.
    if (id[0] == L'-')
    {
        throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, L"'id' may not start with '-'"));
    }

    // The id is forwarded into docker's comma/'='-delimited --secret spec, so reject any character
    // that could break out of the id= field and inject additional options (e.g. ",src=/etc/passwd").
    for (auto ch : id)
    {
        const bool allowed = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                             ch == L'_' || ch == L'-' || ch == L'.';
        if (!allowed)
        {
            throw ArgumentException(
                Localization::MessageWslcSecretInvalidSpec(spec, L"'id' may only contain letters, digits, '_', '-' or '.'"));
        }
    }

    if (!type.empty() && type != L"file" && type != L"env")
    {
        throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unsupported secret type '{}'", type)));
    }

    // Docker parity: 'type=file' names a source file, so it requires 'src='. Without it we would
    // otherwise fall through to reading an environment variable, silently contradicting the type.
    if (type == L"file" && srcPath.empty())
    {
        throw ArgumentException(Localization::MessageWslcSecretInvalidSpec(spec, L"'type=file' requires 'src='"));
    }

    // Docker parity: with 'type=env', a bare 'src=' names the environment variable to read (rather
    // than a file path), unless an explicit 'env=' was also given.
    if (type == L"env" && envName.empty() && !srcPath.empty())
    {
        envName = std::move(srcPath);
        srcPath.clear();
    }

    if (!envName.empty() && !srcPath.empty())
    {
        // Docker parity: 'env=' and 'src=' are not mutually exclusive; when both are given the
        // environment variable wins and the file path is ignored.
        srcPath.clear();
    }
    if (envName.empty() && srcPath.empty())
    {
        // Docker parity: with neither 'env=' nor 'src=', the secret value is read from the host
        // environment variable whose name matches the id. Unlike an explicit 'env=', that variable
        // must be set - Docker errors when the id-named variable is undefined.
        envName = id;
        if (!wsl::windows::common::wslutil::ReadEnvironmentVariable(envName.c_str()).has_value())
        {
            throw ArgumentException(
                Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"environment variable '{}' is not set", envName)));
        }
    }

    if (!srcPath.empty())
    {
        std::error_code ec;
        // Normalize to an absolute path (the service requires one to mount the file's directory) but do
        // not verify the file exists or is a regular file here: that would be a TOCTOU race with the
        // build, and the file may only be reachable from the service's context. Let the service/BuildKit
        // reject an unmountable or unreadable file instead. weakly_canonical resolves a relative path
        // against the current directory, collapses '..', and resolves symlinks for the portion of the
        // path that exists; it succeeds for a missing file but still reports genuine errors.
        auto absPath = std::filesystem::weakly_canonical(srcPath, ec);
        if (ec.value() != 0)
        {
            throw ArgumentException(
                Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"could not resolve source path: {}", srcPath)));
        }

        // Forward the resolved path rather than the bytes: the server mounts the file's parent directory
        // into the build VM read-only and references the file in place with docker's --secret src=, so
        // the secret is never copied off its original (possibly EFS-encrypted) location while still
        // delivering arbitrary binary content byte-for-byte - matching Docker's type=file semantics.
        return services::BuildSecret{
            .Id = std::move(id),
            .SourcePath = absPath.wstring(),
        };
    }

    // Docker parity: a referenced environment variable that is unset (or set but empty) yields an
    // empty secret value rather than an error. ReadEnvironmentVariable returns nullopt for an
    // undefined variable, which we collapse to an empty value.
    const std::wstring value = wsl::windows::common::wslutil::ReadEnvironmentVariable(envName.c_str()).value_or(std::wstring{});

    // The env value is delivered as UTF-8 bytes, matching how the guest exposes it at /run/secrets/<id>.
    auto valueBytes = wsl::windows::common::string::WideToMultiByte(value);
    return services::BuildSecret{
        .Id = std::move(id),
        .Value = std::vector<BYTE>(valueBytes.begin(), valueBytes.end()),
    };
}

services::BuildOutput ParseOutputSpec(const std::wstring& spec)
{
    // Mirrors `docker buildx build --output`. A bare token is shorthand for a destination; otherwise
    // the spec is a single CSV record of key=value pairs where 'type'/'dest' are structural and every
    // other key is forwarded verbatim to buildx as an exporter attribute.
    if (spec.empty())
    {
        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(spec, L"the value may not be empty"));
    }

    // buildx parses the spec as one CSV record (go-csvvalue / encoding/csv): fields are comma
    // separated, a field may be double-quoted, "" inside a quoted field is a literal quote, and a
    // comma inside quotes is part of the value. This lets a value such as an annotation contain commas.
    const auto fields = SplitCsvFields(spec);
    if (!fields.has_value())
    {
        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(spec, L"malformed quoting"));
    }

    // Keys are ASCII and matched case-insensitively, and buildx TrimSpace's each key so " dest=x" after
    // a comma is accepted; values are left untouched. AsciiToLower/TrimAscii live in stringshared.h.

    // Shorthand: a single field that is exactly the input and does not start with "type=" names the
    // destination. Matching Docker, '-' streams a tarball to stdout ('type=tar,dest=-'); anything else
    // is the local (directory) exporter ('type=local,dest=<path>'), which is not supported.
    if (fields->size() == 1 && fields->front() == spec && spec.compare(0, 5, L"type=") != 0)
    {
        if (fields->front() == L"-")
        {
            return services::BuildOutput{.Type = L"tar", .Dest = L"-"};
        }

        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(
            spec,
            L"directory exporters are not supported; write a single file with 'dest=<file>' (type=tar/oci/docker) "
            L"or stream a tarball to stdout with 'type=tar,dest=-'"));
    }

    services::BuildOutput output;
    std::wstring rawType;
    bool hasType = false;

    for (const auto& field : *fields)
    {
        // buildx splits each field on the FIRST '=' and requires two parts; the value is not trimmed.
        const auto pos = field.find(L'=');
        if (pos == std::wstring::npos)
        {
            throw ArgumentException(
                Localization::MessageWslcOutputInvalidSpec(spec, L"expected key=value pairs separated by ','"));
        }

        const auto key = AsciiToLower(TrimAscii(std::wstring_view(field).substr(0, pos)));
        auto value = field.substr(pos + 1);
        if (key.empty())
        {
            throw ArgumentException(
                Localization::MessageWslcOutputInvalidSpec(spec, L"expected key=value pairs separated by ','"));
        }

        if (key == L"type")
        {
            rawType = value;
            output.Type = AsciiToLower(std::wstring_view(value));
            hasType = true;
        }
        else if (key == L"dest")
        {
            output.Dest = std::move(value);
        }
        else
        {
            // Remaining attributes (name, push, compression, tar, annotations, ...) are matched
            // case-insensitively by buildx, so their keys are already lowercased above.
            output.Attributes[key] = std::move(value);
        }
    }

    if (!hasType || output.Type.empty())
    {
        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(spec, L"type is required"));
    }

    // buildx forwards the type to buildkit and only rejects it there; we route the exporter ourselves,
    // so an unroutable type has to be rejected up front. Every real exporter is in this list.
    const bool supportedType = output.Type == L"local" || output.Type == L"tar" || output.Type == L"oci" ||
                               output.Type == L"docker" || output.Type == L"image" || output.Type == L"registry" ||
                               output.Type == L"cacheonly";
    if (!supportedType)
    {
        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(spec, std::format(L"unsupported output type '{}'", rawType)));
    }

    // The 'tar' attribute selects a single tarball vs. an OCI layout directory for oci/docker, and it
    // drives file-vs-directory routing here. buildx parses it with Go's ParseBool and errors on
    // anything else, so reject an invalid value up front rather than failing confusingly in the VM.
    if (output.Type == L"oci" || output.Type == L"docker")
    {
        const auto tarIt = output.Attributes.find(L"tar");
        if (tarIt != output.Attributes.end() && !ParseBool(tarIt->second.c_str(), true).has_value())
        {
            throw ArgumentException(
                Localization::MessageWslcOutputInvalidSpec(spec, std::format(L"invalid boolean value '{}' for 'tar'", tarIt->second)));
        }
    }

    // Destination resolution, mirroring `docker buildx build --output`:
    if (OutputIsDirectory(output))
    {
        // Directory exporters (local, or oci/docker with tar=false) write a Linux directory tree, which
        // cannot be materialized faithfully on a Windows destination, so they are not supported. Point
        // users at the single-file exporters instead.
        throw ArgumentException(Localization::MessageWslcOutputInvalidSpec(
            spec,
            L"directory exporters are not supported; write a single file with 'dest=<file>' (type=tar/oci/docker) "
            L"or stream a tarball to stdout with 'type=tar,dest=-'"));
    }

    if (output.Type == L"tar" || output.Type == L"oci")
    {
        // Single-tarball exporters stream to stdout when no destination is given (buildx default).
        if (output.Dest.empty())
        {
            output.Dest = L"-";
        }
    }
    // docker: no dest -> load into the VM image store (leave empty); dest='-' streams a tarball to
    //         stdout; a path writes a file. image/registry/cacheonly run in the VM and ignore 'dest'.

    return output;
}

bool OutputStreamsToClient(const services::BuildOutput& output)
{
    if (output.Type == L"local" || output.Type == L"tar" || output.Type == L"oci")
    {
        return true;
    }

    if (output.Type == L"docker")
    {
        // An omitted destination loads the image into the store in the VM; any destination (a file or
        // stdout '-') is produced in the VM and streamed back to the client.
        return !output.Dest.empty();
    }

    // image / registry / cacheonly run entirely in the build VM.
    return false;
}

bool OutputIsDirectory(const services::BuildOutput& output)
{
    if (output.Type == L"local")
    {
        return true;
    }

    if (output.Type == L"oci" || output.Type == L"docker")
    {
        // oci/docker default to a single tarball but export an OCI layout directory when tar is false.
        const auto it = output.Attributes.find(L"tar");
        if (it != output.Attributes.end())
        {
            const auto tar = ParseBool(it->second.c_str(), true);
            return tar.has_value() && !tar.value();
        }
    }

    return false;
}

std::wstring FormatOutputSpec(const services::BuildOutput& output)
{
    // buildx consumes the same CSV key=value form we parsed, so we round-trip the parsed struct back
    // into a canonical spec. This is what actually reaches `docker build --output <spec>`. Each token
    // is CSV-escaped so a value containing a comma or quote survives the trip.
    std::vector<std::wstring> fields;
    fields.push_back(std::format(L"type={}", output.Type));
    if (!output.Dest.empty())
    {
        fields.push_back(std::format(L"dest={}", output.Dest));
    }

    for (const auto& [key, value] : output.Attributes)
    {
        fields.push_back(std::format(L"{}={}", key, value));
    }

    return JoinCsvFields(fields);
}

std::tuple<std::string, int64_t, int64_t> ParseUlimit(const std::wstring& input, const std::wstring& argName)
{
    // Accepts <name>=<soft>[:<hard>]; if hard is omitted hard = soft. -1 means unlimited.
    const auto nameValue = SplitKeyValue(input);
    if (!nameValue.HadSeparator || nameValue.Key.empty())
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
    }

    const std::wstring& valuesPart = nameValue.Value;
    const auto colonPos = valuesPart.find(L':');

    auto parseLimit = [&](const std::wstring& limitStr) -> int64_t {
        if (limitStr.empty())
        {
            throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
        }

        try
        {
            return GetIntegerFromString<int64_t>(limitStr, argName, [](int64_t v) { return v >= -1; });
        }
        catch (const ArgumentException&)
        {
            // Re-throw with the ulimit-specific error message so the user sees the full input.
            throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
        }
    };

    const int64_t soft = parseLimit(colonPos == std::wstring::npos ? valuesPart : valuesPart.substr(0, colonPos));
    const int64_t hard = colonPos == std::wstring::npos ? soft : parseLimit(valuesPart.substr(colonPos + 1));

    // This rejects "-1:1024" and "-1:<finite>" while allowing "<finite>:-1", "-1:-1", and "-1".
    const bool invalidRange = (soft == -1) ? (hard != -1) : (hard != -1 && hard < soft);
    if (invalidRange)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
    }

    return {WideToMultiByte(nameValue.Key), soft, hard};
}

std::pair<std::string, std::string> ParseLabel(const std::wstring& value)
{
    const auto kv = SplitKeyValue(value);
    auto key = WideToMultiByte(kv.Key);
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_LabelKeyEmptyError(), key.empty());
    return {std::move(key), WideToMultiByte(kv.Value)};
}

std::pair<std::string, std::string> ParseDriverOption(const std::wstring& value)
{
    const auto kv = SplitKeyValue(value);
    return {WideToMultiByte(kv.Key), WideToMultiByte(kv.Value)};
}

std::pair<std::string, std::string> ParseFilter(const std::wstring& value)
{
    const auto kv = SplitKeyValue(value);
    if (!kv.HadSeparator)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidFilterError(value));
    }

    return {WideToMultiByte(kv.Key), WideToMultiByte(kv.Value)};
}

// Map of signal names to WSLCSignal enum values
static const std::unordered_map<std::wstring, WSLCSignal> SignalMap = {
    {L"SIGHUP", WSLCSignalSIGHUP},   {L"SIGINT", WSLCSignalSIGINT},     {L"SIGQUIT", WSLCSignalSIGQUIT},
    {L"SIGILL", WSLCSignalSIGILL},   {L"SIGTRAP", WSLCSignalSIGTRAP},   {L"SIGABRT", WSLCSignalSIGABRT},
    {L"SIGIOT", WSLCSignalSIGIOT},   {L"SIGBUS", WSLCSignalSIGBUS},     {L"SIGFPE", WSLCSignalSIGFPE},
    {L"SIGKILL", WSLCSignalSIGKILL}, {L"SIGUSR1", WSLCSignalSIGUSR1},   {L"SIGSEGV", WSLCSignalSIGSEGV},
    {L"SIGUSR2", WSLCSignalSIGUSR2}, {L"SIGPIPE", WSLCSignalSIGPIPE},   {L"SIGALRM", WSLCSignalSIGALRM},
    {L"SIGTERM", WSLCSignalSIGTERM}, {L"SIGTKFLT", WSLCSignalSIGTKFLT}, {L"SIGCHLD", WSLCSignalSIGCHLD},
    {L"SIGCONT", WSLCSignalSIGCONT}, {L"SIGSTOP", WSLCSignalSIGSTOP},   {L"SIGTSTP", WSLCSignalSIGTSTP},
    {L"SIGTTIN", WSLCSignalSIGTTIN}, {L"SIGTTOU", WSLCSignalSIGTTOU},   {L"SIGURG", WSLCSignalSIGURG},
    {L"SIGXCPU", WSLCSignalSIGXCPU}, {L"SIGXFSZ", WSLCSignalSIGXFSZ},   {L"SIGVTALRM", WSLCSignalSIGVTALRM},
    {L"SIGPROF", WSLCSignalSIGPROF}, {L"SIGWINCH", WSLCSignalSIGWINCH}, {L"SIGIO", WSLCSignalSIGIO},
    {L"SIGPOLL", WSLCSignalSIGPOLL}, {L"SIGPWR", WSLCSignalSIGPWR},     {L"SIGSYS", WSLCSignalSIGSYS},
};

// Convert string to WSLCSignal enum - accepts either signal name (e.g., "SIGKILL") or number (e.g., "9")
WSLCSignal GetWSLCSignalFromString(const std::wstring& input, const std::wstring& argName)
{
    constexpr int MIN_SIGNAL = WSLCSignalSIGHUP;
    constexpr int MAX_SIGNAL = WSLCSignalSIGSYS;
    constexpr std::wstring_view sigPrefix = L"SIG";

    // Normalize input: ensure it has "SIG" prefix for map lookup
    std::wstring normalizedInput;
    if (IsEqual(input.substr(0, sigPrefix.size()), sigPrefix, true))
    {
        normalizedInput = input;
    }
    else
    {
        normalizedInput = std::wstring(sigPrefix) + input;
    }

    for (const auto& [signalName, signalValue] : SignalMap)
    {
        if (IsEqual(normalizedInput, signalName, true))
        {
            return signalValue;
        }
    }

    // User may have input an integer representation instead.
    int signalValue{};
    try
    {
        signalValue = GetIntegerFromString<int>(input, argName);
    }
    // If it fails to be converted give a better user message than just the integer conversion
    // failure since we also know it failed to be found in the map.
    catch (ArgumentException)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidSignalError(argName, input));
    }

    if (signalValue < MIN_SIGNAL || signalValue > MAX_SIGNAL)
    {
        throw ArgumentException(Localization::WSLCCLI_SignalOutOfRangeError(argName, input, MIN_SIGNAL, MAX_SIGNAL));
    }

    return static_cast<WSLCSignal>(signalValue);
}

// Parses an RFC3339 timestamp (e.g. "2024-01-15T10:30:00Z" or "2024-01-15T10:30:00+05:30")
// into a ULONGLONG Unix epoch seconds value using std::chrono::parse.
// Note: +HHMM (no colon) offsets are not supported; use +HH:MM format.
static std::optional<ULONGLONG> TryParseRfc3339(const std::string& input)
{
    std::string normalized = input;

    // Normalize trailing 'Z'/'z' to '+00:00' so %Ez can parse it uniformly.
    if (!normalized.empty() && (normalized.back() == 'Z' || normalized.back() == 'z'))
    {
        normalized.pop_back();
        normalized += "+00:00";
    }

    // Reject bare dot with no fractional digits (e.g. "10:30:00.+00:00") since
    // std::chrono::parse is lenient about this.
    auto dotPos = normalized.find('.');
    if (dotPos != std::string::npos && (dotPos + 1 >= normalized.size() || !std::isdigit(normalized[dotPos + 1])))
    {
        return std::nullopt;
    }

    // Pre-validate day-of-month since std::chrono::parse silently wraps invalid dates (e.g. Feb 31 → Mar 2).
    if (normalized.size() >= 10 && normalized[4] == '-' && normalized[7] == '-')
    {
        int year = 0, month = 0, day = 0;
        auto yResult = std::from_chars(normalized.data(), normalized.data() + 4, year);
        auto mResult = std::from_chars(normalized.data() + 5, normalized.data() + 7, month);
        auto dResult = std::from_chars(normalized.data() + 8, normalized.data() + 10, day);

        if (yResult.ec == std::errc() && mResult.ec == std::errc() && dResult.ec == std::errc())
        {
            auto ymd = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                       std::chrono::day{static_cast<unsigned>(day)};
            if (!ymd.ok())
            {
                return std::nullopt;
            }
        }
    }

    // Parse into nanosecond precision so fractional seconds (e.g. ".123456789") are consumed
    // by std::chrono::parse rather than requiring manual stripping.
    std::chrono::sys_time<std::chrono::nanoseconds> utcTime;
    std::istringstream stream(normalized);
    stream >> std::chrono::parse("%FT%T%Ez", utcTime);
    if (stream.fail())
    {
        return std::nullopt;
    }

    // Reject if there are trailing characters after the parsed timestamp
    if (stream.peek() != std::istringstream::traits_type::eof())
    {
        return std::nullopt;
    }

    auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(utcTime.time_since_epoch()).count();
    if (epochSeconds < 0)
    {
        return std::nullopt;
    }

    return static_cast<ULONGLONG>(epochSeconds);
}

ULONGLONG GetTimestampFromString(const std::wstring& value, const std::wstring& argName)
{
    std::string narrowValue = wsl::windows::common::string::WideToMultiByte(value);

    // Try integer (Unix epoch seconds) first
    ULONGLONG intValue{};
    const char* begin = narrowValue.c_str();
    const char* end = begin + narrowValue.size();
    auto result = std::from_chars(begin, end, intValue);
    if (result.ec == std::errc() && result.ptr == end)
    {
        return intValue;
    }

    // Try RFC3339 timestamp
    auto rfc3339Value = TryParseRfc3339(narrowValue);
    if (rfc3339Value.has_value())
    {
        return rfc3339Value.value();
    }

    throw ArgumentException(Localization::WSLCCLI_InvalidTimestampArgumentError(argName, value));
}

models::FormatType GetFormatTypeFromString(const std::wstring& input, const std::wstring& argName)
{
    // Single source of truth for the accepted format values. It drives both parsing and the error
    // message's supported-values list, so adding a type here updates both automatically.
    static constexpr std::pair<std::wstring_view, models::FormatType> c_formatTypes[] = {
        {L"json", models::FormatType::Json},
        {L"table", models::FormatType::Table},
    };

    for (const auto& [name, type] : c_formatTypes)
    {
        if (IsEqual(input, name))
        {
            return type;
        }
    }

    std::wstring supportedValues;
    for (const auto& formatType : c_formatTypes)
    {
        if (!supportedValues.empty())
        {
            supportedValues += L", ";
        }

        supportedValues += formatType.first;
    }

    throw ArgumentException(Localization::WSLCCLI_InvalidFormatValueError(argName, input, supportedValues));
}

int GetInspectJsonIndentFromString(const std::wstring& input, const std::wstring& argName)
{
    if (!IsEqual(input, L"json"))
    {
        constexpr std::wstring_view supportedValues = L"json";
        throw ArgumentException(Localization::WSLCCLI_InvalidFormatValueError(argName, input, supportedValues));
    }

    return wsl::shared::c_jsonCompactIndent;
}

models::PullPolicy GetPullPolicyFromString(const std::wstring& input, const std::wstring& argName)
{
    static constexpr std::pair<std::wstring_view, models::PullPolicy> c_pullPolicies[] = {
        {L"always", models::PullPolicy::Always},
        {L"missing", models::PullPolicy::Missing},
        {L"never", models::PullPolicy::Never},
    };

    for (const auto& [name, policy] : c_pullPolicies)
    {
        if (IsEqual(input, name))
        {
            return policy;
        }
    }

    std::wstring supportedValues;
    for (const auto& pullPolicy : c_pullPolicies)
    {
        if (!supportedValues.empty())
        {
            supportedValues += L", ";
        }

        supportedValues += pullPolicy.first;
    }

    throw ArgumentException(Localization::WSLCCLI_InvalidPullPolicyError(argName, input, supportedValues));
}

models::ProgressMode GetProgressModeFromString(const std::wstring& input, const std::wstring& argName)
{
    if (IsEqual(input, L"auto"))
    {
        return models::ProgressMode::Auto;
    }
    else if (IsEqual(input, L"tty"))
    {
        return models::ProgressMode::Tty;
    }
    else if (IsEqual(input, L"plain"))
    {
        return models::ProgressMode::Plain;
    }
    else if (IsEqual(input, L"quiet"))
    {
        return models::ProgressMode::Quiet;
    }
    else if (IsEqual(input, L"rawjson"))
    {
        return models::ProgressMode::RawJson;
    }
    else
    {
        throw ArgumentException(std::format(
            L"Invalid {} value: {} is not a recognized progress type. Supported progress types are: auto, tty, plain, "
            L"quiet, rawjson.",
            argName,
            input));
    }
}

models::InspectType GetInspectTypeFromString(const std::wstring& input, const std::wstring& argName)
{
    if (IsEqual(input, L"image"))
    {
        return models::InspectType::Image;
    }
    else if (IsEqual(input, L"container"))
    {
        return models::InspectType::Container;
    }
    else if (IsEqual(input, L"network"))
    {
        return models::InspectType::Network;
    }
    else if (IsEqual(input, L"volume"))
    {
        return models::InspectType::Volume;
    }
    else
    {
        constexpr std::wstring_view supportedValues = L"image, container, network, volume";
        throw ArgumentException(Localization::WSLCCLI_InvalidInspectError(argName, input, supportedValues));
    }
}

int64_t GetMemorySizeFromString(const std::wstring& input, const std::wstring& argName)
{
    auto parsed = wsl::shared::string::ParseMemorySize(input.c_str());
    if (!parsed.has_value())
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidMemorySizeError(argName, input));
    }

    return static_cast<int64_t>(parsed.value());
}

// Parses duration string into nanoseconds.
static std::optional<int64_t> TryParseDuration(const std::string& input)
{
    if (input.empty())
    {
        return std::nullopt;
    }

    size_t pos = 0;
    bool negative = false;
    if (input[pos] == '+' || input[pos] == '-')
    {
        negative = input[pos] == '-';
        pos++;
    }

    // Special case: a bare "0" (with optional sign) is a valid zero duration.
    if (input.substr(pos) == "0")
    {
        return 0;
    }

    // Accumulate in a long double so fractional units (e.g. "1.5h") are handled, then round.
    long double totalNanos = 0.0L;
    bool sawValue = false;

    while (pos < input.size())
    {
        // Parse the numeric part (integer and/or fraction).
        const size_t numberStart = pos;
        while (pos < input.size() && (std::isdigit(static_cast<unsigned char>(input[pos])) || input[pos] == '.'))
        {
            pos++;
        }

        const std::string numberStr = input.substr(numberStart, pos - numberStart);
        if (numberStr.empty() || numberStr == "." || std::count(numberStr.begin(), numberStr.end(), '.') > 1)
        {
            return std::nullopt;
        }

        // Parse the unit (everything up to the next digit or '.').
        const size_t unitStart = pos;
        while (pos < input.size() && !std::isdigit(static_cast<unsigned char>(input[pos])) && input[pos] != '.')
        {
            pos++;
        }

        const std::string unit = input.substr(unitStart, pos - unitStart);

        long double multiplier{};
        if (unit == "ns")
        {
            multiplier = 1.0L;
        }
        else if (unit == "us" || unit == "\xC2\xB5s" /* µs (U+00B5) */ || unit == "\xCE\xBCs" /* μs (U+03BC) */)
        {
            multiplier = 1000L;
        }
        else if (unit == "ms")
        {
            multiplier = 1000000L;
        }
        else if (unit == "s")
        {
            multiplier = 1000000000L;
        }
        else if (unit == "m")
        {
            multiplier = 60000000000L;
        }
        else if (unit == "h")
        {
            multiplier = 3600000000000L;
        }
        else
        {
            return std::nullopt;
        }

        long double value{};
        try
        {
            auto [ptr, ec] = std::from_chars(numberStr.data(), numberStr.data() + numberStr.size(), value, std::chars_format::fixed);
            if (ptr != numberStr.data() + numberStr.size() || ec != std::errc())
            {
                return std::nullopt;
            }
        }
        catch (...)
        {
            return std::nullopt;
        }

        totalNanos += value * multiplier;
        sawValue = true;
    }

    if (!sawValue)
    {
        return std::nullopt;
    }

    if (negative)
    {
        totalNanos = -totalNanos;
    }

    if (totalNanos > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        totalNanos < static_cast<long double>(std::numeric_limits<int64_t>::min()))
    {
        return std::nullopt;
    }

    return static_cast<int64_t>(std::llroundl(totalNanos));
}

int64_t GetDurationNanosFromString(const std::wstring& input, const std::wstring& argName)
{
    const std::string narrow = WideToMultiByte(input);
    const auto parsed = TryParseDuration(narrow);

    if (!parsed.has_value() || parsed.value() < 0)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidDurationError(argName, input));
    }

    return parsed.value();
}

int64_t GetNanoCpusFromString(const std::wstring& input, const std::wstring& argName)
{
    constexpr double NanosPerCpu = 1'000'000'000.0;
    constexpr double MaxCpus = static_cast<double>(std::numeric_limits<int64_t>::max()) / NanosPerCpu;

    const std::string narrow = WideToMultiByte(input);
    const char* begin = narrow.c_str();
    const char* end = begin + narrow.size();

    double cpus{};
    const auto result = std::from_chars(begin, end, cpus, std::chars_format::fixed);
    if (result.ec != std::errc() || result.ptr != end || cpus <= 0.0 || cpus > MaxCpus)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidCpusError(argName, input));
    }

    return static_cast<int64_t>(cpus * NanosPerCpu);
}

} // namespace wsl::windows::wslc::validation
