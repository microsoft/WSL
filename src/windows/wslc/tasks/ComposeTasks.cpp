// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "ComposeService.h"
#include "ComposeTasks.h"

using namespace wsl::windows::wslc::argument;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::shared;

namespace wsl::windows::wslc::task {

namespace {

    std::wstring ComposePath(CLIExecutionContext& context)
    {
        return std::filesystem::absolute(context.Args.GetValue<ArgType::Path>()).wstring();
    }

    ComposeProjectReference ComposeProject(CLIExecutionContext& context)
    {
        return ComposeProjectReference{std::filesystem::path{ComposePath(context)}};
    }

    template <ArgType E>
    void ThrowIfSupplied(ArgMap& args)
    {
        bool supplied = false;
        if constexpr (std::is_same_v<ArgMap::value_t<E>, bool>)
        {
            supplied = args.GetValue<E>();
        }
        else
        {
            supplied = args.Contains(E);
        }

        if (!supplied)
        {
            return;
        }

        const auto name = std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + Argument::Create(E).Name();
        THROW_HR_WITH_USER_ERROR(E_NOTIMPL, Localization::WSLCCLI_ComposeOptionNotImplementedError(name));
    }

    template <ArgType... Types>
    void ThrowIfAnySupplied(ArgMap& args)
    {
        (ThrowIfSupplied<Types>(args), ...);
    }

} // namespace

void AttachCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode = ComposeService::Attach(context.Terminal, session, ComposeProject(context), context.CreateCancelEvent());
}

void CreateCompose(CLIExecutionContext& context)
{
    ComposeService::Create(context.Terminal, context.Data.Get<Data::Session>(), ComposePath(context), context.CreateCancelEvent());
}

void StartCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Start(context.Terminal, session, ComposeProject(context), context.CreateCancelEvent());
}

void StopCompose(CLIExecutionContext& context)
{
    constexpr LONG c_defaultTimeout = 10;
    const LONG timeout = context.Args.Contains(ArgType::Time) ? context.Args.GetValue<ArgType::Time>() : c_defaultTimeout;
    THROW_HR_IF(E_INVALIDARG, timeout < 0);

    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Stop(context.Terminal, session, ComposeProject(context), static_cast<ULONG>(timeout), context.CreateCancelEvent());
}

void UpCompose(CLIExecutionContext& context)
{
    context.ExitCode =
        ComposeService::Up(context.Terminal, context.Data.Get<Data::Session>(), ComposePath(context), context.CreateCancelEvent());
}

void ValidateComposeOptions(CLIExecutionContext& context)
{
    ThrowIfAnySupplied<
        ArgType::ComposeAllResources,
        ArgType::ComposeAnsi,
        ArgType::ComposeCompatibility,
        ArgType::ComposeGlobalDryRun,
        ArgType::ComposeEnvFile,
        ArgType::ComposeFile,
        ArgType::ComposeParallel,
        ArgType::ComposeProfile,
        ArgType::ComposeProgress,
        ArgType::ComposeProjectDirectory,
        ArgType::ComposeProjectName,
        ArgType::ComposeDryRun>(context.Args);
}

} // namespace wsl::windows::wslc::task
