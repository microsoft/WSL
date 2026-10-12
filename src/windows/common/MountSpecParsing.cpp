/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    MountSpecParsing.cpp

Abstract:

    Docker-compatible mount specification parsing.

--*/

#include "precomp.h"
#include "MountSpecParsing.h"
#include "string.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <regex>
#include <unordered_set>
#include <vector>

using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::common::mount {

namespace {

    enum class Field
    {
        Type,
        Source,
        Target,
        ReadOnly,
        Consistency,
        BindPropagation,
        BindNonRecursive,
        BindRecursive,
        VolumeNoCopy,
        VolumeLabel,
        VolumeDriver,
        VolumeOption,
        TmpfsSize,
        TmpfsMode,
    };

    enum class Family
    {
        General,
        Bind,
        Volume,
        Tmpfs,
    };

    enum class Support
    {
        Supported,
        Unsupported,
        ValueDependent,
    };

    struct FieldDefinition
    {
        std::wstring_view Name;
        Field Id;
        Family OptionFamily;
        bool AllowsBareForm;
        Support SupportLevel;
    };

    // Keep this table aligned with docker/cli v25.0.3 opts/mount.go. It is the
    // authoritative list of accepted fields, aliases, option families, and WSLC support.
    constexpr std::array c_fieldDefinitions{
        FieldDefinition{L"type", Field::Type, Family::General, false, Support::ValueDependent},
        FieldDefinition{L"source", Field::Source, Family::General, false, Support::Supported},
        FieldDefinition{L"src", Field::Source, Family::General, false, Support::Supported},
        FieldDefinition{L"target", Field::Target, Family::General, false, Support::Supported},
        FieldDefinition{L"dst", Field::Target, Family::General, false, Support::Supported},
        FieldDefinition{L"destination", Field::Target, Family::General, false, Support::Supported},
        FieldDefinition{L"readonly", Field::ReadOnly, Family::General, true, Support::Supported},
        FieldDefinition{L"ro", Field::ReadOnly, Family::General, true, Support::Supported},
        // The WSLC mount transport and Docker request model have no end-to-end consistency setting.
        FieldDefinition{L"consistency", Field::Consistency, Family::General, false, Support::Unsupported},
        // The WSLC mount transport and Docker request model do not carry Docker bind options.
        // "enabled" requires no non-default bind behavior and is accepted below.
        FieldDefinition{L"bind-propagation", Field::BindPropagation, Family::Bind, false, Support::Unsupported},
        FieldDefinition{L"bind-nonrecursive", Field::BindNonRecursive, Family::Bind, true, Support::Unsupported},
        FieldDefinition{L"bind-recursive", Field::BindRecursive, Family::Bind, false, Support::ValueDependent},
        // The mount pipeline relies on Docker's default volume copy-up behavior and does not carry VolumeOptions.
        FieldDefinition{L"volume-nocopy", Field::VolumeNoCopy, Family::Volume, true, Support::Unsupported},
        // Inline mounts reference volumes by name; the volume creation API owns labels, drivers, and driver options.
        FieldDefinition{L"volume-label", Field::VolumeLabel, Family::Volume, false, Support::Unsupported},
        FieldDefinition{L"volume-driver", Field::VolumeDriver, Family::Volume, false, Support::Unsupported},
        FieldDefinition{L"volume-opt", Field::VolumeOption, Family::Volume, false, Support::Unsupported},
        FieldDefinition{L"tmpfs-size", Field::TmpfsSize, Family::Tmpfs, false, Support::Supported},
        FieldDefinition{L"tmpfs-mode", Field::TmpfsMode, Family::Tmpfs, false, Support::Supported},
    };

    struct DockerMountSpec
    {
        std::wstring Type = L"volume";
        std::wstring Source;
        std::wstring Target;
        bool ReadOnly = false;
        bool HasVolumeOptions = false;
        bool HasBindOptions = false;
        bool HasTmpfsOptions = false;
        bool BindReadOnlyNonRecursive = false;
        bool BindReadOnlyForceRecursive = false;
        std::wstring BindPropagation;
        std::optional<int64_t> TmpfsSizeBytes;
        std::optional<uint32_t> TmpfsMode;
        std::optional<std::wstring> UnsupportedOption;
    };

    struct KeyValue
    {
        std::wstring Key;
        std::wstring Value;
        bool HadSeparator;
    };

    [[noreturn]] void ThrowValidation(std::wstring reason)
    {
        throw MountValidationException(std::move(reason));
    }

    [[noreturn]] void ThrowParse(std::wstring reason)
    {
        throw MountParseException(std::move(reason));
    }

    [[noreturn]] void ThrowUnsupported(std::wstring reason)
    {
        throw MountUnsupportedException(std::move(reason));
    }

    KeyValue SplitKeyValue(const std::wstring& value)
    {
        const auto position = value.find(L'=');
        if (position == std::wstring::npos)
        {
            return {.Key = value, .HadSeparator = false};
        }

        return {.Key = value.substr(0, position), .Value = value.substr(position + 1), .HadSeparator = true};
    }

    const FieldDefinition* FindField(std::wstring_view name)
    {
        const auto found = std::ranges::find_if(c_fieldDefinitions, [&](const auto& definition) { return definition.Name == name; });
        return found == c_fieldDefinitions.end() ? nullptr : &*found;
    }

    void RecordUnsupportedOption(DockerMountSpec& mount, std::wstring_view option)
    {
        if (!mount.UnsupportedOption.has_value())
        {
            mount.UnsupportedOption = option;
        }
    }

    std::optional<int64_t> ParseDockerRamInBytes(const std::wstring& value)
    {
        const auto parsed = wsl::windows::common::string::ParseStorageSize(value, wsl::windows::common::string::StorageSizeUnit::Binary);
        if (!parsed.has_value() || parsed.value() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return std::nullopt;
        }

        return static_cast<int64_t>(parsed.value());
    }

    std::optional<uint32_t> ParseDockerTmpfsMode(const std::wstring& value)
    {
        if (value.empty())
        {
            return std::nullopt;
        }

        uint64_t result = 0;
        for (const auto digit : value)
        {
            if (digit < L'0' || digit > L'7')
            {
                return std::nullopt;
            }

            result = (result * 8) + static_cast<uint64_t>(digit - L'0');
            if (result > std::numeric_limits<uint32_t>::max())
            {
                return std::nullopt;
            }
        }

        return static_cast<uint32_t>(result);
    }

    std::string FormatDockerTmpfsSize(int64_t sizeBytes)
    {
        for (const auto& [suffix, divisor] : std::array<std::pair<char, int64_t>, 3>{{{'g', 1LL << 30}, {'m', 1LL << 20}, {'k', 1LL << 10}}})
        {
            if ((sizeBytes % divisor) == 0)
            {
                return std::format("{}{}", sizeBytes / divisor, suffix);
            }
        }

        return std::to_string(sizeBytes);
    }

} // namespace

Spec ParseDockerMountString(const std::wstring& value)
{
    const auto fields = SplitCsvFields(value);
    if (!fields.has_value())
    {
        ThrowParse(Localization::WSLCCLI_MountMalformedCsvError());
    }

    DockerMountSpec mount;

    for (const auto& field : *fields)
    {
        const auto keyValue = SplitKeyValue(field);
        const auto key = AsciiToLower(std::wstring_view(keyValue.Key));
        const auto definition = FindField(key);
        if (definition == nullptr)
        {
            if (!keyValue.HadSeparator)
            {
                ThrowParse(Localization::WSLCCLI_MountFieldKeyValueRequiredError(field));
            }

            ThrowParse(Localization::WSLCCLI_MountUnexpectedKeyError(key, field));
        }

        if (!keyValue.HadSeparator && !definition->AllowsBareForm)
        {
            ThrowParse(Localization::WSLCCLI_MountFieldKeyValueRequiredError(field));
        }

        switch (definition->OptionFamily)
        {
        case Family::General:
            break;
        case Family::Bind:
            if (definition->Id != Field::BindRecursive || keyValue.Value != L"enabled")
            {
                mount.HasBindOptions = true;
            }
            break;
        case Family::Volume:
            mount.HasVolumeOptions = true;
            break;
        case Family::Tmpfs:
            mount.HasTmpfsOptions = true;
            break;
        }

        switch (definition->Id)
        {
        case Field::Type:
            mount.Type = AsciiToLower(std::wstring_view(keyValue.Value));
            break;

        case Field::Source:
            mount.Source = keyValue.Value;
            if (mount.Source == L"." || mount.Source.starts_with(L".\\"))
            {
                std::error_code error;
                auto absolutePath = std::filesystem::absolute(mount.Source, error);
                if (!error)
                {
                    mount.Source = absolutePath.lexically_normal().wstring();
                }
            }
            break;

        case Field::Target:
            mount.Target = keyValue.Value;
            break;

        case Field::ReadOnly:
            if (!keyValue.HadSeparator)
            {
                mount.ReadOnly = true;
                break;
            }

            if (const auto parsed = ParseBool(keyValue.Value.c_str(), true); parsed.has_value())
            {
                mount.ReadOnly = parsed.value();
            }
            else
            {
                ThrowParse(Localization::WSLCCLI_MountInvalidValueError(key, keyValue.Value));
            }
            break;

        case Field::Consistency:
            break;

        case Field::BindPropagation:
            mount.BindPropagation = AsciiToLower(std::wstring_view(keyValue.Value));
            break;

        case Field::BindNonRecursive:
            if (keyValue.HadSeparator && !ParseBool(keyValue.Value.c_str(), true).has_value())
            {
                ThrowParse(Localization::WSLCCLI_MountInvalidValueError(key, keyValue.Value));
            }

            break;

        case Field::BindRecursive:
            if (keyValue.Value == L"enabled")
            {
                break;
            }

            RecordUnsupportedOption(mount, key);
            if (keyValue.Value == L"disabled")
            {
                break;
            }
            if (keyValue.Value == L"writable")
            {
                mount.BindReadOnlyNonRecursive = true;
                break;
            }
            if (keyValue.Value == L"readonly")
            {
                mount.BindReadOnlyForceRecursive = true;
                break;
            }

            ThrowParse(Localization::WSLCCLI_MountInvalidBindRecursiveValueError(key, keyValue.Value));

        case Field::VolumeNoCopy:
            if (keyValue.HadSeparator && !ParseBool(keyValue.Value.c_str(), true).has_value())
            {
                ThrowParse(Localization::WSLCCLI_MountInvalidValueError(L"volume-nocopy", keyValue.Value));
            }

            break;

        case Field::VolumeLabel:
        case Field::VolumeDriver:
        case Field::VolumeOption:
            break;

        case Field::TmpfsSize:
            mount.TmpfsSizeBytes = ParseDockerRamInBytes(keyValue.Value);
            if (!mount.TmpfsSizeBytes.has_value())
            {
                ThrowParse(Localization::WSLCCLI_MountInvalidValueError(key, keyValue.Value));
            }

            break;

        case Field::TmpfsMode:
            mount.TmpfsMode = ParseDockerTmpfsMode(keyValue.Value);
            if (!mount.TmpfsMode.has_value())
            {
                ThrowParse(Localization::WSLCCLI_MountInvalidValueError(key, keyValue.Value));
            }

            break;
        }

        if (definition->SupportLevel == Support::Unsupported)
        {
            RecordUnsupportedOption(mount, key);
        }
    }

    if (mount.Type.empty())
    {
        ThrowParse(Localization::WSLCCLI_MountTypeRequiredError());
    }

    if (mount.HasVolumeOptions && mount.Type != L"volume")
    {
        ThrowParse(Localization::WSLCCLI_MountOptionFamilyMismatchError(L"volume-*", mount.Type));
    }
    if (mount.HasBindOptions && mount.Type != L"bind")
    {
        ThrowParse(Localization::WSLCCLI_MountOptionFamilyMismatchError(L"bind-*", mount.Type));
    }
    if (mount.HasTmpfsOptions && mount.Type != L"tmpfs")
    {
        ThrowParse(Localization::WSLCCLI_MountOptionFamilyMismatchError(L"tmpfs-*", mount.Type));
    }

    if (mount.BindReadOnlyNonRecursive && !mount.ReadOnly)
    {
        ThrowParse(Localization::WSLCCLI_MountOptionRequiresReadonlyError(L"bind-recursive=writable"));
    }
    if (mount.BindReadOnlyForceRecursive)
    {
        if (!mount.ReadOnly)
        {
            ThrowParse(Localization::WSLCCLI_MountOptionRequiresReadonlyError(L"bind-recursive=readonly"));
        }
        if (mount.BindPropagation != L"rprivate")
        {
            ThrowParse(Localization::WSLCCLI_MountBindRecursiveReadonlyRequiresPropagationError());
        }
    }

    Type type;
    if (mount.Type == L"bind")
    {
        type = WSLCMountTypeBind;
    }
    else if (mount.Type == L"volume")
    {
        type = WSLCMountTypeVolume;
    }
    else if (mount.Type == L"tmpfs")
    {
        type = WSLCMountTypeTmpfs;
    }
    else
    {
        ThrowUnsupported(Localization::WSLCCLI_MountTypeUnsupportedError(mount.Type));
    }

    if (mount.UnsupportedOption.has_value())
    {
        ThrowUnsupported(Localization::WSLCCLI_MountOptionUnsupportedError(mount.UnsupportedOption.value()));
    }

    return {
        .MountType = type,
        .Source = std::move(mount.Source),
        .Target = WideToMultiByte(mount.Target),
        .ReadOnly = mount.ReadOnly,
        .TmpfsSizeBytes = mount.TmpfsSizeBytes,
        .TmpfsMode = mount.TmpfsMode,
    };
}

Spec ParseDockerVolumeString(const std::wstring& value)
{
    const auto formatUsage = Localization::WSLCCLI_VolumeFormatUsage();
    const auto lastColon = value.rfind(L':');
    if (lastColon == std::wstring::npos)
    {
        ThrowParse(Localization::WSLCCLI_VolumeInvalidSpec(value, formatUsage));
    }

    auto splitColon = lastColon;
    bool readOnly = false;
    std::wstring_view lastToken{value.data() + lastColon + 1, value.size() - lastColon - 1};
    if (lastToken == L"ro" || lastToken == L"rw")
    {
        readOnly = lastToken == L"ro";
        if (lastColon == 0)
        {
            ThrowParse(Localization::WSLCCLI_VolumeInvalidSpec(value, formatUsage));
        }

        splitColon = value.rfind(L':', lastColon - 1);
        if (splitColon == std::wstring::npos)
        {
            ThrowParse(Localization::WSLCCLI_VolumeInvalidSpec(value, formatUsage));
        }
    }

    const auto targetEnd = lastToken == L"ro" || lastToken == L"rw" ? lastColon : value.size();
    const auto target = value.substr(splitColon + 1, targetEnd - splitColon - 1);
    if (target.empty())
    {
        ThrowParse(Localization::WSLCCLI_VolumeContainerPathEmpty(value, formatUsage));
    }

    if (target.front() != L'/')
    {
        ThrowParse(Localization::WSLCCLI_VolumeContainerPathNotAbsolute(value, formatUsage));
    }

    const auto rawSource = value.substr(0, splitColon);
    if (rawSource.empty())
    {
        ThrowParse(Localization::WSLCCLI_VolumeHostPathEmpty(value, formatUsage));
    }

    if (IsValidNamedVolumeName(rawSource))
    {
        return {
            .MountType = WSLCMountTypeVolume,
            .Source = rawSource,
            .Target = WideToMultiByte(target),
            .ReadOnly = readOnly,
        };
    }

    std::error_code error;
    auto source = wsl::windows::common::filesystem::GetCanonicalPath(rawSource, error);
    if (error)
    {
        ThrowParse(Localization::WSLCCLI_VolumeHostPathInvalid(value, rawSource));
    }

    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_INVALID_NAME)
    {
        ThrowParse(Localization::WSLCCLI_VolumeHostPathInvalid(value, rawSource));
    }

    return {
        .MountType = WSLCMountTypeBind,
        .Source = source.wstring(),
        .Target = WideToMultiByte(target),
        .ReadOnly = readOnly,
        .BindSource = BindSourcePolicy::CreateIfMissing,
    };
}

Spec ParseDockerTmpfsString(const std::wstring& value)
{
    const auto colon = value.find(L':');
    const auto target = value.substr(0, colon);
    const auto options = colon == std::wstring::npos ? std::wstring_view{} : std::wstring_view{value}.substr(colon + 1);

    return {
        .MountType = WSLCMountTypeTmpfs,
        .Target = WideToMultiByte(target),
        .TmpfsOptions = WideToMultiByte(std::wstring{options}),
    };
}

void ValidateMountSpec(const Spec& mount)
{
    if (mount.Target.empty())
    {
        ThrowValidation(Localization::WSLCCLI_MountTargetRequiredError());
    }

    if (!mount.Target.starts_with('/'))
    {
        ThrowValidation(Localization::WSLCCLI_MountTargetAbsoluteError());
    }

    if (mount.MountType != WSLCMountTypeTmpfs &&
        (mount.TmpfsSizeBytes.has_value() || mount.TmpfsMode.has_value() || mount.TmpfsOptions.has_value()))
    {
        ThrowValidation(Localization::WSLCCLI_MountTmpfsOptionsTypeError());
    }

    if (mount.TmpfsSizeBytes.has_value() && mount.TmpfsSizeBytes.value() < 0)
    {
        ThrowValidation(Localization::WSLCCLI_MountTmpfsSizeNegativeError());
    }

    switch (mount.MountType)
    {
    case WSLCMountTypeBind:
        if (mount.Source.empty())
        {
            ThrowValidation(Localization::WSLCCLI_MountSourceRequiredError());
        }

        if (!std::filesystem::path(mount.Source).is_absolute())
        {
            ThrowValidation(Localization::WSLCCLI_MountBindSourceAbsoluteError());
        }
        break;

    case WSLCMountTypeVolume:
        if (!mount.Source.empty() && !IsValidNamedVolumeName(mount.Source))
        {
            ThrowValidation(Localization::WSLCCLI_MountVolumeSourceInvalidError());
        }
        break;

    case WSLCMountTypeTmpfs:
        if (!mount.Source.empty())
        {
            ThrowValidation(Localization::WSLCCLI_MountTmpfsSourceUnsupportedError());
        }
        break;

    default:
        ThrowUnsupported(Localization::WSLCCLI_MountTypeUnsupportedGenericError());
    }
}

void ValidateMountCollection(std::span<const Spec> mounts)
{
    std::unordered_set<std::string> destinations;
    for (const auto& mount : mounts)
    {
        ValidateMountSpec(mount);

        auto destination = NormalizeDestination(mount.Target);
        if (!destinations.emplace(destination).second)
        {
            throw MountValidationException(
                ValidationError::DuplicateDestination,
                Localization::WSLCCLI_DuplicateMountDestinationError(MultiByteToWide(destination)),
                std::move(destination));
        }
    }
}

std::string FormatTmpfsOptions(const Spec& mount)
{
    WI_ASSERT(mount.MountType == WSLCMountTypeTmpfs);

    if (mount.TmpfsOptions.has_value())
    {
        return mount.TmpfsOptions.value();
    }

    std::vector<std::string> options;
    if (mount.ReadOnly)
    {
        options.emplace_back("ro");
    }
    if (mount.TmpfsMode.has_value() && mount.TmpfsMode.value() != 0)
    {
        options.emplace_back(std::format("mode={:o}", mount.TmpfsMode.value()));
    }
    if (mount.TmpfsSizeBytes.has_value() && mount.TmpfsSizeBytes.value() != 0)
    {
        options.emplace_back(std::format("size={}", FormatDockerTmpfsSize(mount.TmpfsSizeBytes.value())));
    }

    return wsl::shared::string::Join<char>(options, ',');
}

std::string NormalizeDestination(std::string destination)
{
    std::vector<std::string> components;
    size_t start = 0;
    while (start <= destination.size())
    {
        const auto end = destination.find('/', start);
        const auto component = destination.substr(start, end - start);
        if (!component.empty() && component != ".")
        {
            if (component == "..")
            {
                if (!components.empty())
                {
                    components.pop_back();
                }
            }
            else
            {
                components.emplace_back(component);
            }
        }

        if (end == std::string::npos)
        {
            break;
        }

        start = end + 1;
    }

    std::string result = "/";
    for (const auto& component : components)
    {
        if (result.size() > 1)
        {
            result += '/';
        }

        result += component;
    }

    return result;
}

bool IsValidNamedVolumeName(std::wstring_view name)
{
    static const std::wregex c_namedVolumeRegex(LR"(^[a-zA-Z0-9][a-zA-Z0-9_.-]{1,}$)");
    return std::regex_match(name.begin(), name.end(), c_namedVolumeRegex);
}

} // namespace wsl::windows::common::mount
