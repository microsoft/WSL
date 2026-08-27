/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.cpp

--*/
#include "precomp.h"
#include "EnvironmentOptions.h"

namespace wsl::windows::wslc {
namespace {

    // nullopt iff the variable is not defined; engaged (possibly empty) otherwise.
    std::optional<std::wstring> ReadEnv(const wchar_t* name)
    {
        std::wstring value;
        const HRESULT hr = wil::GetEnvironmentVariableW(name, value);
        if (hr == HRESULT_FROM_WIN32(ERROR_ENVVAR_NOT_FOUND))
        {
            return std::nullopt;
        }

        THROW_IF_FAILED(hr);
        return value;
    }

} // namespace

void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs) noexcept
try
{
    for (const auto& arg : definedArgs)
    {
        // Lowest-precedence: skip args already set by the caller.
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
CATCH_LOG()

} // namespace wsl::windows::wslc
