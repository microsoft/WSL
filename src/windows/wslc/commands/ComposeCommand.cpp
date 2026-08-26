/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeCommand.cpp

Abstract:

    Implements the minimal compose command tree.

--*/

#include "precomp.h"
#include "ArgumentConvertedTypes.h"
#include "ComposeCommand.h"
#include "ComposeService.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::services;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

namespace {

    std::vector<Argument> ComposePathArguments(bool IncludeTimeout = false)
    {
        std::vector<Argument> arguments{
            Argument::Create(ArgType::Path, true, std::nullopt, Localization::WSLCCLI_ComposePathArgDescription()),
        };
        if (IncludeTimeout)
        {
            arguments.emplace_back(Argument::Create(ArgType::Time));
        }

        return arguments;
    }

    models::Session& ResolveComposeSession(CLIExecutionContext& Context)
    {
        Context << ResolveSession;
        return Context.Data.Get<Data::Session>();
    }

    std::wstring ComposePath(CLIExecutionContext& Context)
    {
        return std::filesystem::absolute(Context.Args.GetValue<ArgType::Path>()).wstring();
    }

} // namespace

std::vector<std::unique_ptr<Command>> ComposeCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ComposeCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeUpCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStartCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeAttachCommand>(FullName()));
    commands.push_back(std::make_unique<ComposeStopCommand>(FullName()));
    return commands;
}

std::wstring ComposeCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeCommandDesc();
}

std::wstring ComposeCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeCommandLongDesc();
}

void ComposeCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    OutputHelp(Context.Terminal);
}

std::vector<Argument> ComposeCreateCommand::GetArguments() const
{
    return ComposePathArguments();
}

std::wstring ComposeCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeCreateDesc();
}

std::wstring ComposeCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeCreateLongDesc();
}

void ComposeCreateCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    ComposeService::Create(ResolveComposeSession(Context), ComposePath(Context));
}

std::vector<Argument> ComposeUpCommand::GetArguments() const
{
    return ComposePathArguments();
}

std::wstring ComposeUpCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeUpDesc();
}

std::wstring ComposeUpCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeUpLongDesc();
}

void ComposeUpCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context.ExitCode = ComposeService::Up(Context.Terminal, ResolveComposeSession(Context), ComposePath(Context));
}

std::vector<Argument> ComposeStartCommand::GetArguments() const
{
    return ComposePathArguments();
}

std::wstring ComposeStartCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeStartDesc();
}

std::wstring ComposeStartCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeStartLongDesc();
}

void ComposeStartCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    ComposeService::Start(ResolveComposeSession(Context), ComposePath(Context));
}

std::vector<Argument> ComposeAttachCommand::GetArguments() const
{
    return ComposePathArguments();
}

std::wstring ComposeAttachCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeAttachDesc();
}

std::wstring ComposeAttachCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeAttachLongDesc();
}

void ComposeAttachCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context.ExitCode = ComposeService::Attach(Context.Terminal, ResolveComposeSession(Context), ComposePath(Context));
}

std::vector<Argument> ComposeStopCommand::GetArguments() const
{
    return ComposePathArguments(true);
}

std::wstring ComposeStopCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeStopDesc();
}

std::wstring ComposeStopCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeStopLongDesc();
}

void ComposeStopCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    constexpr LONG c_defaultTimeout = 10;
    const LONG timeout = Context.Args.Contains(ArgType::Time) ? Context.Args.GetValue<ArgType::Time>() : c_defaultTimeout;
    THROW_HR_IF(E_INVALIDARG, timeout < 0);
    ComposeService::Stop(ResolveComposeSession(Context), ComposePath(Context), static_cast<ULONG>(timeout));
}

} // namespace wsl::windows::wslc
