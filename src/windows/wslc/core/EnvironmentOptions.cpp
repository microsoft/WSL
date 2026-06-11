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

    // Many-to-one allowed: list multiple bindings for the same ArgType to give
    // users alternate spellings (e.g. NO_COLOR alongside WSLC_CLI_NO_COLOR).
    constexpr EnvBinding c_bindings[] = {
        {L"WSLC_CLI_DEBUG", ArgType::Debug},
        {L"WSLC_CLI_NO_COLOR", ArgType::NoColor},
        {L"NO_COLOR", ArgType::NoColor},
        {L"WSLC_CLI_VERBOSE", ArgType::Verbose},
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

    // Follows the NO_COLOR convention: any value is truthy, with explicit
    // opt-out values so the var can be set-but-disabled without unsetting it.
    bool IsTruthy(const std::wstring& v)
    {
        if (v.empty())
        {
            return false;
        }

        auto eq = [&](const wchar_t* s) { return _wcsicmp(v.c_str(), s) == 0; };
        return !(eq(L"0") || eq(L"false") || eq(L"no") || eq(L"off"));
    }

    const Argument* FindArg(const std::vector<Argument>& defs, ArgType type)
    {
        for (const auto& a : defs)
        {
            if (a.Type() == type)
            {
                return &a;
            }
        }
        return nullptr;
    }

} // namespace

void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs)
{
    for (const auto& b : c_bindings)
    {
        if (target.Contains(b.Type))
        {
            continue;
        }

        const Argument* arg = FindArg(definedArgs, b.Type);
        if (!arg)
        {
            continue;
        }

        auto value = ReadEnv(b.Name);
        if (!value.has_value())
        {
            continue;
        }

        switch (arg->Kind())
        {
        case Kind::Flag:
            if (IsTruthy(*value))
            {
                target.Add(b.Type, true);
            }
            break;
        case Kind::Value:
            target.Add(b.Type, std::move(*value));
            break;
        case Kind::Positional:
        case Kind::Forward:
            break;
        }
    }
}

} // namespace wsl::windows::wslc
