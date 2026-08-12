/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    MountSpecParsing.h

Abstract:

    Docker-compatible mount specification parsing.

--*/

#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wsl::windows::common::mount {

inline constexpr std::string_view c_dockerCliMountGrammarVersion = "25.0.3";

enum class Type
{
    Bind,
    Volume,
    Tmpfs,
};

struct Spec
{
    Type MountType = Type::Volume;
    std::wstring Source;
    std::string Target;
    bool ReadOnly = false;
    std::optional<int64_t> TmpfsSizeBytes;
    std::optional<uint32_t> TmpfsMode;
};

class ParseException : public std::exception
{
public:
    explicit ParseException(std::wstring reason) : m_reason(std::move(reason))
    {
    }

    const char* what() const noexcept override
    {
        return "invalid mount specification";
    }

    const std::wstring& Reason() const noexcept
    {
        return m_reason;
    }

private:
    std::wstring m_reason;
};

Spec Parse(const std::wstring& value);
std::string FormatTmpfsOptions(const Spec& mount);
std::string NormalizeDestination(std::string destination);
bool IsValidNamedVolumeName(std::wstring_view name);

} // namespace wsl::windows::common::mount
