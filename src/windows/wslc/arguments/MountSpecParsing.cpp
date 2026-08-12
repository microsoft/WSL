/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    MountSpecParsing.cpp

Abstract:

    Parser for Docker-style --mount specifications.

--*/

#include "precomp.h"
#include "MountSpecParsing.h"
#include "ContainerModel.h"
#include "Exceptions.h"
#include "Localization.h"
#include "SpecParsing.h"
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>

using namespace wsl::windows::common;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc::validation {

namespace {

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

    [[noreturn]] void ThrowInvalidMount(const std::wstring& spec, const std::wstring& reason)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidMountError(spec, reason));
    }

    void RecordUnsupportedOption(DockerMountSpec& mount, const std::wstring& option)
    {
        if (!mount.UnsupportedOption.has_value())
        {
            mount.UnsupportedOption = option;
        }
    }

    std::optional<int64_t> ParseDockerRamInBytes(const std::wstring& value)
    {
        const auto input = WideToMultiByte(value);
        const auto separator = input.find_last_of("01234567890. ");
        if (separator == std::string::npos)
        {
            return std::nullopt;
        }

        std::string number;
        std::string suffix;
        if (input[separator] == ' ')
        {
            number = input.substr(0, separator);
            suffix = input.substr(separator + 1);
        }
        else
        {
            number = input.substr(0, separator + 1);
            suffix = input.substr(separator + 1);
        }

        if (number.empty() || std::isspace(static_cast<unsigned char>(number.front())))
        {
            return std::nullopt;
        }

        std::string_view numberView(number);
        if (numberView.front() == '+')
        {
            numberView.remove_prefix(1);
            if (numberView.empty())
            {
                return std::nullopt;
            }
        }

        double parsed{};
        const auto parseResult =
            std::from_chars(numberView.data(), numberView.data() + numberView.size(), parsed, std::chars_format::general);
        if (parseResult.ec != std::errc() || parseResult.ptr != numberView.data() + numberView.size() || !std::isfinite(parsed) || parsed < 0)
        {
            return std::nullopt;
        }

        double bytes = parsed;
        if (!suffix.empty())
        {
            suffix = AsciiToLower(std::string_view(suffix));
            if (suffix.size() > 3)
            {
                return std::nullopt;
            }

            if (suffix.front() == 'b')
            {
                if (suffix.size() != 1)
                {
                    return std::nullopt;
                }
            }
            else
            {
                uint64_t factor{};
                switch (suffix.front())
                {
                case 'k':
                    factor = 1ULL << 10;
                    break;
                case 'm':
                    factor = 1ULL << 20;
                    break;
                case 'g':
                    factor = 1ULL << 30;
                    break;
                case 't':
                    factor = 1ULL << 40;
                    break;
                case 'p':
                    factor = 1ULL << 50;
                    break;
                default:
                    return std::nullopt;
                }

                if ((suffix.size() == 2 && suffix[1] != 'b') || (suffix.size() == 3 && suffix.substr(1) != "ib"))
                {
                    return std::nullopt;
                }

                bytes *= static_cast<double>(factor);
            }
        }

        constexpr double c_int64Limit = 9223372036854775808.0;
        if (!std::isfinite(bytes) || bytes >= c_int64Limit)
        {
            return std::nullopt;
        }

        return static_cast<int64_t>(bytes);
    }

    std::optional<uint32_t> ParseDockerTmpfsMode(const std::wstring& value)
    {
        if (value.empty() || value.front() == L'-')
        {
            return std::nullopt;
        }

        size_t position = value.front() == L'+' ? 1 : 0;
        if (position == value.size())
        {
            return std::nullopt;
        }

        uint64_t result = 0;
        for (; position < value.size(); ++position)
        {
            const auto digit = value[position];
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

ParsedMount ParseMount(const std::wstring& value)
{
    // Keep this parser aligned with docker/cli v25.0.3 opts/mount.go. If the bundled Docker
    // backend is updated, revisit both the parsing rules and the WSLC support gate below.
    const auto fields = SplitCsvFields(value);
    if (!fields.has_value())
    {
        ThrowInvalidMount(value, L"malformed CSV");
    }

    DockerMountSpec mount;

    for (const auto& field : *fields)
    {
        const auto keyValue = SplitKeyValue(field);
        const auto key = AsciiToLower(std::wstring_view(keyValue.Key));

        if (!keyValue.HadSeparator)
        {
            if (key == L"readonly" || key == L"ro")
            {
                mount.ReadOnly = true;
                continue;
            }

            if (key == L"volume-nocopy")
            {
                mount.HasVolumeOptions = true;
                RecordUnsupportedOption(mount, key);
                continue;
            }

            if (key == L"bind-nonrecursive")
            {
                mount.HasBindOptions = true;
                RecordUnsupportedOption(mount, key);
                continue;
            }

            ThrowInvalidMount(value, std::format(L"invalid field '{}' must be a key=value pair", field));
        }

        if (key == L"type")
        {
            mount.Type = AsciiToLower(std::wstring_view(keyValue.Value));
        }
        else if (key == L"source" || key == L"src")
        {
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
        }
        else if (key == L"target" || key == L"dst" || key == L"destination")
        {
            mount.Target = keyValue.Value;
        }
        else if (key == L"readonly" || key == L"ro")
        {
            const auto parsed = ParseBool(keyValue.Value.c_str(), true);
            if (!parsed.has_value())
            {
                ThrowInvalidMount(value, std::format(L"invalid value for {}: {}", key, keyValue.Value));
            }

            mount.ReadOnly = parsed.value();
        }
        else if (key == L"consistency")
        {
            RecordUnsupportedOption(mount, key);
        }
        else if (key == L"bind-propagation")
        {
            mount.HasBindOptions = true;
            mount.BindPropagation = AsciiToLower(std::wstring_view(keyValue.Value));
            RecordUnsupportedOption(mount, key);
        }
        else if (key == L"bind-nonrecursive")
        {
            if (!ParseBool(keyValue.Value.c_str(), true).has_value())
            {
                ThrowInvalidMount(value, std::format(L"invalid value for {}: {}", key, keyValue.Value));
            }

            mount.HasBindOptions = true;
            RecordUnsupportedOption(mount, key);
        }
        else if (key == L"bind-recursive")
        {
            if (keyValue.Value == L"enabled")
            {
                continue;
            }

            mount.HasBindOptions = true;
            RecordUnsupportedOption(mount, key);
            if (keyValue.Value == L"disabled")
            {
                continue;
            }
            if (keyValue.Value == L"writable")
            {
                mount.BindReadOnlyNonRecursive = true;
                continue;
            }
            if (keyValue.Value == L"readonly")
            {
                mount.BindReadOnlyForceRecursive = true;
                continue;
            }

            ThrowInvalidMount(
                value,
                std::format(
                    L"invalid value for {}: {} (must be \"enabled\", \"disabled\", \"writable\", or \"readonly\")", key, keyValue.Value));
        }
        else if (key == L"volume-nocopy")
        {
            if (!ParseBool(keyValue.Value.c_str(), true).has_value())
            {
                ThrowInvalidMount(value, std::format(L"invalid value for volume-nocopy: {}", keyValue.Value));
            }

            mount.HasVolumeOptions = true;
            RecordUnsupportedOption(mount, key);
        }
        else if (key == L"volume-label" || key == L"volume-driver" || key == L"volume-opt")
        {
            mount.HasVolumeOptions = true;
            RecordUnsupportedOption(mount, key);
        }
        else if (key == L"tmpfs-size")
        {
            mount.TmpfsSizeBytes = ParseDockerRamInBytes(keyValue.Value);
            if (!mount.TmpfsSizeBytes.has_value())
            {
                ThrowInvalidMount(value, std::format(L"invalid value for {}: {}", key, keyValue.Value));
            }

            mount.HasTmpfsOptions = true;
        }
        else if (key == L"tmpfs-mode")
        {
            mount.TmpfsMode = ParseDockerTmpfsMode(keyValue.Value);
            if (!mount.TmpfsMode.has_value())
            {
                ThrowInvalidMount(value, std::format(L"invalid value for {}: {}", key, keyValue.Value));
            }

            mount.HasTmpfsOptions = true;
        }
        else
        {
            ThrowInvalidMount(value, std::format(L"unexpected key '{}' in '{}'", key, field));
        }
    }

    if (mount.Type.empty())
    {
        ThrowInvalidMount(value, L"type is required");
    }

    if (mount.Target.empty())
    {
        ThrowInvalidMount(value, L"target is required");
    }

    if (mount.HasVolumeOptions && mount.Type != L"volume")
    {
        ThrowInvalidMount(value, std::format(L"cannot mix 'volume-*' options with mount type '{}'", mount.Type));
    }
    if (mount.HasBindOptions && mount.Type != L"bind")
    {
        ThrowInvalidMount(value, std::format(L"cannot mix 'bind-*' options with mount type '{}'", mount.Type));
    }
    if (mount.HasTmpfsOptions && mount.Type != L"tmpfs")
    {
        ThrowInvalidMount(value, std::format(L"cannot mix 'tmpfs-*' options with mount type '{}'", mount.Type));
    }

    if (mount.BindReadOnlyNonRecursive && !mount.ReadOnly)
    {
        ThrowInvalidMount(value, L"option 'bind-recursive=writable' requires 'readonly' to be specified in conjunction");
    }
    if (mount.BindReadOnlyForceRecursive)
    {
        if (!mount.ReadOnly)
        {
            ThrowInvalidMount(value, L"option 'bind-recursive=readonly' requires 'readonly' to be specified in conjunction");
        }
        if (mount.BindPropagation != L"rprivate")
        {
            ThrowInvalidMount(
                value, L"option 'bind-recursive=readonly' requires 'bind-propagation=rprivate' to be specified in conjunction");
        }
    }

    if (mount.Type != L"bind" && mount.Type != L"volume" && mount.Type != L"tmpfs")
    {
        ThrowInvalidMount(value, std::format(L"mount type '{}' is not supported by WSLC", mount.Type));
    }
    if (mount.UnsupportedOption.has_value())
    {
        ThrowInvalidMount(value, std::format(L"option '{}' is not supported by WSLC", mount.UnsupportedOption.value()));
    }
    if (mount.Target.find(L':') != std::wstring::npos)
    {
        ThrowInvalidMount(value, L"target paths containing ':' are not supported by WSLC");
    }

    ParsedMount result;
    if (mount.Type == L"tmpfs")
    {
        if (!mount.Source.empty())
        {
            ThrowInvalidMount(value, L"source is not supported for tmpfs mounts");
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

        result.IsTmpfs = true;
        result.TmpfsSpec = WideToMultiByte(mount.Target);
        if (!options.empty())
        {
            result.TmpfsSpec += ":" + wsl::shared::string::Join<char>(options, ',');
        }
    }
    else
    {
        if (mount.Source.empty())
        {
            if (mount.Type == L"volume")
            {
                ThrowInvalidMount(value, L"anonymous volume mounts are not supported by WSLC");
            }

            ThrowInvalidMount(value, L"source is required");
        }

        if (mount.Type == L"bind" && !std::filesystem::path(mount.Source).is_absolute())
        {
            ThrowInvalidMount(value, L"bind source path must be absolute");
        }
        if (mount.Type == L"volume" && !models::VolumeMount::IsValidNamedVolumeName(mount.Source))
        {
            ThrowInvalidMount(value, L"volume source must be a valid named volume");
        }

        result.VolumeSpec = mount.Source + L":" + mount.Target;
        if (mount.ReadOnly)
        {
            result.VolumeSpec += L":ro";
        }

        const auto parsed = models::VolumeMount::Parse(result.VolumeSpec);
        if ((mount.Type == L"bind" && parsed.IsNamedVolume()) || (mount.Type == L"volume" && !parsed.IsNamedVolume()))
        {
            ThrowInvalidMount(value, std::format(L"source is not valid for mount type '{}'", mount.Type));
        }
    }

    return result;
}

} // namespace wsl::windows::wslc::validation
