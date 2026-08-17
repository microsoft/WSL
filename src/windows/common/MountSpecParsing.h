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
#include <span>
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

enum class BindSourcePolicy
{
    RequireExisting,
    CreateIfMissing,
};

struct Spec
{
    Type MountType = Type::Volume;
    std::wstring Source;
    std::string Target;
    bool ReadOnly = false;
    BindSourcePolicy BindSource = BindSourcePolicy::RequireExisting;
    std::optional<int64_t> TmpfsSizeBytes;
    std::optional<uint32_t> TmpfsMode;
};

enum class ValidationError
{
    InvalidSpecification,
    DuplicateDestination,
};

class MountException : public std::exception
{
public:
    explicit MountException(std::wstring reason) : m_reason(std::move(reason))
    {
    }

    MountException(ValidationError error, std::wstring reason, std::string destination) :
        m_error(error), m_reason(std::move(reason)), m_destination(std::move(destination))
    {
    }

    const char* what() const noexcept override
    {
        return "mount error";
    }

    const std::wstring& Reason() const noexcept
    {
        return m_reason;
    }

    ValidationError Error() const noexcept
    {
        return m_error;
    }

    const std::string& Destination() const noexcept
    {
        return m_destination;
    }

private:
    ValidationError m_error = ValidationError::InvalidSpecification;
    std::wstring m_reason;
    std::string m_destination;
};

class MountParseException : public MountException
{
public:
    using MountException::MountException;
};

class MountUnsupportedException : public MountException
{
public:
    using MountException::MountException;
};

class MountValidationException : public MountException
{
public:
    using MountException::MountException;
};

Spec ParseDockerMountString(const std::wstring& value);
Spec ParseDockerVolumeString(const std::wstring& value);
void ValidateMountSpec(const Spec& mount);
void ValidateMountCollection(std::span<const Spec> mounts);
std::string FormatTmpfsOptions(const Spec& mount);
std::string NormalizeDestination(std::string destination);
bool IsValidNamedVolumeName(std::wstring_view name);

} // namespace wsl::windows::common::mount
