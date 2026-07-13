/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RootCommand.cpp

Abstract:

    Implementation of the RootCommand, which is the root of all commands in the CLI.

--*/
#include "RootCommand.h"

// Include all commands that parent to the root.
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "NetworkCommand.h"
#include "RegistryCommand.h"
#include "SettingsCommand.h"
#include "SystemCommand.h"
#include "InspectCommand.h"
#include "VersionCommand.h"
#include "VolumeCommand.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ContainerCommand>(FullName()));
    commands.push_back(std::make_unique<ImageCommand>(FullName()));
    commands.push_back(std::make_unique<NetworkCommand>(FullName()));
    commands.push_back(std::make_unique<RegistryCommand>(FullName()));
    commands.push_back(std::make_unique<SettingsCommand>(FullName()));
    commands.push_back(std::make_unique<SystemCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerAttachCommand>(FullName()));
    commands.push_back(std::make_unique<ImageBuildCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerExecCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerExportCommand>(FullName()));
    commands.push_back(std::make_unique<ImageListCommand>(FullName(), true));
    commands.push_back(std::make_unique<ImageImportCommand>(FullName()));
    commands.push_back(std::make_unique<InspectCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerKillCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerListCommand>(FullName()));
    commands.push_back(std::make_unique<ImageLoadCommand>(FullName()));
    commands.push_back(std::make_unique<RegistryLoginCommand>(FullName()));
    commands.push_back(std::make_unique<RegistryLogoutCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerLogsCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePullCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePushCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerRemoveCommand>(FullName()));
    commands.push_back(std::make_unique<ImageRemoveCommand>(FullName(), true));
    commands.push_back(std::make_unique<ContainerRunCommand>(FullName()));
    commands.push_back(std::make_unique<ImageSaveCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStartCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStatsCommand>(FullName()));
    commands.push_back(std::make_unique<ContainerStopCommand>(FullName()));
    commands.push_back(std::make_unique<ImageTagCommand>(FullName()));
    commands.push_back(std::make_unique<VersionCommand>(FullName()));
    return commands;
}

std::vector<Argument> RootCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Version),
    };
}

// Global options apply to the overall invocation and may appear before any
// subcommand (e.g. `wslc --session foo image list`). Define them here using
// the Argument::Create factory backed by ArgumentDefinitions.h so help text,
// aliases, validation, and parsing match subcommand arguments.
std::vector<Argument> RootCommand::GetGlobalArguments() const
{
    return {
        Argument::Create(ArgType::Session),
    };
}

std::vector<Argument> RootCommand::GetEnvArguments() const
{
    return {
        Argument::Create(ArgType::NoColor),
    };
}

std::wstring RootCommand::ShortDescription() const
{
    return Localization::WSLCCLI_RootCommandDesc();
}

std::wstring RootCommand::LongDescription() const
{
    return Localization::WSLCCLI_RootCommandLongDesc();
}

void RootCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (context.Args.Contains(ArgType::Version))
    {
        VersionCommand::PrintVersion(context.Reporter);
        return;
    }

    OutputHelp(context.Reporter);
}
} // namespace wsl::windows::wslc
