/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.cpp

--*/
#include "precomp.h"
#include "EnvironmentOptions.h"

namespace wsl::windows::wslc {

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

            auto value = wsl::windows::common::wslutil::ReadEnvironmentVariable(binding.Name);
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
