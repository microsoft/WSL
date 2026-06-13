/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.cpp

--*/
#include "precomp.h"
#include "EnvironmentOptions.h"

namespace wsl::windows::wslc {
namespace {

    // Returns the env var value when defined (including empty), nullopt
    // otherwise. Noexcept: swallows allocation failure as "no value".
    //
    // wil::GetEnvironmentVariableW returns a FAILED HRESULT when the variable
    // is not present in the process environment (mapped from
    // ERROR_ENVVAR_NOT_FOUND). A defined-but-empty variable returns S_OK and
    // an empty string, which we surface as an engaged optional so the caller
    // can honor presence-only contracts (e.g. NO_COLOR=).
    std::optional<std::wstring> ReadEnv(const wchar_t* name) noexcept
    try
    {
        std::wstring value;
        const HRESULT hr = wil::GetEnvironmentVariableW(name, value);
        if (FAILED(hr))
        {
            return std::nullopt;
        }
        return value;
    }
    catch (...)
    {
        return std::nullopt;
    }

} // namespace

void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs) noexcept
try
{
    for (const auto& arg : definedArgs)
    {
        // Defensive: skip entries already populated so this can run either
        // before or after CLI parsing without double-adding values.
        if (target.Contains(arg.Type()))
        {
            continue;
        }

        for (const auto& binding : c_envBindings)
        {
            if (binding.Type != arg.Type())
            {
                continue;
            }

            auto value = ReadEnv(binding.Name);
            if (!value.has_value())
            {
                continue;
            }

            // Presence sets a flag; values are stored verbatim. Argument
            // Kinds other than Flag/Value are not env-bindable today.
            if (arg.Kind() == Kind::Flag)
            {
                target.Add(arg.Type(), true);
            }
            else if (arg.Kind() == Kind::Value)
            {
                target.Add(arg.Type(), std::move(*value));
            }

            break;
        }
    }
}
catch (...)
{
    // Hard contract: must not throw out of this function. It runs before
    // NO_COLOR is applied, so a throw could surface as colored help/error
    // output from the parser's error path.
    LOG_CAUGHT_EXCEPTION();
}

} // namespace wsl::windows::wslc
