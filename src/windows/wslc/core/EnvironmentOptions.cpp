/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.cpp

--*/
#include "precomp.h"
#include "EnvironmentOptions.h"

namespace wsl::windows::wslc {
namespace {

    struct EnvBinding
    {
        const wchar_t* Name;
        ArgType Type;
    };

    // Many-to-one allowed: alternate spellings for the same ArgType (e.g. NO_COLOR and WSLC_CLI_NO_COLOR).
    constexpr EnvBinding c_bindings[] = {
        {L"WSLC_CLI_DEBUG", ArgType::Debug},
        {L"WSLC_CLI_NO_COLOR", ArgType::NoColor},
        {L"NO_COLOR", ArgType::NoColor},
    };

    std::optional<std::wstring> ReadEnv(const wchar_t* name)
    {
        DWORD len = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (len == 0)
        {
            return std::nullopt;
        }

        std::wstring value(len - 1, L'\0');
        DWORD got = ::GetEnvironmentVariableW(name, value.data(), len);
        if (got == 0 || got >= len)
        {
            return std::nullopt;
        }
        return value;
    }

    // NO_COLOR convention: any value is truthy except explicit opt-outs.
    bool IsTruthy(const std::wstring& v)
    {
        if (v.empty())
        {
            return false;
        }

        auto eq = [&](const wchar_t* s) { return _wcsicmp(v.c_str(), s) == 0; };
        return !(eq(L"0") || eq(L"false") || eq(L"no") || eq(L"off"));
    }

} // namespace

void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs)
{
    // Iterate by arg so per-ArgType policy lives in one place.
    for (const auto& arg : definedArgs)
    {
        // CLI wins over environment.
        if (target.Contains(arg.Type()))
        {
            continue;
        }

        std::vector<std::wstring> values;
        for (const auto& b : c_bindings)
        {
            if (b.Type != arg.Type())
            {
                continue;
            }

            auto v = ReadEnv(b.Name);
            if (v.has_value())
            {
                values.push_back(std::move(*v));
            }
        }

        if (values.empty())
        {
            continue;
        }

        switch (arg.Kind())
        {
        case Kind::Flag:
        {
            // OR across all bindings: any truthy value sets the flag.
            bool any = false;
            for (const auto& v : values)
            {
                if (IsTruthy(v))
                {
                    any = true;
                    break;
                }
            }
            if (any)
            {
                target.Add(arg.Type(), true);
            }
            break;
        }
        case Kind::Value:
            // TODO: no defined policy for multiple value bindings; first wins for now.
            target.Add(arg.Type(), std::move(values.front()));
            break;
        default:
            // Programming error: these kinds are not supported for environment binding.
            THROW_HR_MSG(E_UNEXPECTED, "ApplyEnvironmentOptions: ArgType %u is not supported.", static_cast<unsigned>(arg.Type()));
        }
    }
}

} // namespace wsl::windows::wslc
