/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.cpp

--*/
#include "precomp.h"
#include "EnvironmentOptions.h"

namespace wsl::windows::wslc {
namespace {

    // Noexcept: swallows allocation failure as "no value" rather than propagating.
    // Note: distinguishes neither "missing" nor "empty" from caller's perspective;
    // both yield nullopt. Use IsEnvPresent() when presence alone matters.
    std::optional<std::wstring> ReadEnv(const wchar_t* name) noexcept
    try
    {
        DWORD len = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (len == 0)
        {
            return std::nullopt;
        }

        std::wstring value(len - 1, L'\0');
        DWORD got = ::GetEnvironmentVariableW(name, value.data(), len);
        if (got >= len)
        {
            return std::nullopt;
        }
        return value;
    }
    catch (...)
    {
        return std::nullopt;
    }

    // Presence-only check: returns true if the env var is defined in the
    // process environment, regardless of value (including empty).
    bool IsEnvPresent(const wchar_t* name) noexcept
    {
        ::SetLastError(ERROR_SUCCESS);
        (void)::GetEnvironmentVariableW(name, nullptr, 0);
        return ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }

    // Vendor-specific convention: any value is truthy except explicit opt-outs.
    // NOT used for NO_COLOR — that binding is presence-only per spec.
    bool IsTruthy(const std::wstring& v) noexcept
    {
        if (v.empty())
        {
            return false;
        }

        auto eq = [&](const wchar_t* s) { return _wcsicmp(v.c_str(), s) == 0; };
        return !(eq(L"0") || eq(L"false") || eq(L"no") || eq(L"off"));
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

        switch (arg.Kind())
        {
        case Kind::Flag:
        {
            // OR across bindings: any matching env var sets the flag.
            // Presence-only bindings ignore the value; truthy-gated bindings
            // require IsTruthy() to accept the value.
            bool any = false;
            for (const auto& b : c_envBindings)
            {
                if (b.Type != arg.Type())
                {
                    continue;
                }

                if (b.PresenceOnly)
                {
                    if (IsEnvPresent(b.Name))
                    {
                        any = true;
                        break;
                    }
                }
                else
                {
                    auto v = ReadEnv(b.Name);
                    if (v.has_value() && IsTruthy(*v))
                    {
                        any = true;
                        break;
                    }
                }
            }
            if (any)
            {
                target.Add(arg.Type(), true);
            }
            break;
        }
        case Kind::Value:
        {
            // TODO: no defined policy for multiple value bindings; first wins.
            // PresenceOnly is ignored for value kinds (it has no sensible meaning
            // when the value is the payload).
            for (const auto& b : c_envBindings)
            {
                if (b.Type != arg.Type())
                {
                    continue;
                }

                auto v = ReadEnv(b.Name);
                if (v.has_value())
                {
                    target.Add(arg.Type(), std::move(*v));
                    break;
                }
            }
            break;
        }
        default:
            // Programming error (not user input): an env-bound Argument was
            // declared with a Kind that cannot be sourced from the environment.
            // Fail fast so the bad declaration is caught in development.
            FAIL_FAST_HR_MSG(E_UNEXPECTED, "ApplyEnvironmentOptions: ArgType %u Kind is not supported.", static_cast<unsigned>(arg.Type()));
        }
    }
}
catch (...)
{
    // Hard contract: user input / environment must not produce a throw out of
    // this function. Anything that lands here (e.g. OOM during Add) is logged
    // and swallowed so the early color/debug setup cannot itself emit colored
    // output via the parser's error path.
    LOG_CAUGHT_EXCEPTION();
}

} // namespace wsl::windows::wslc
