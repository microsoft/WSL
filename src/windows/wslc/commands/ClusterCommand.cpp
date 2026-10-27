/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ClusterCommand.cpp

Abstract:

    Standalone developer cluster command tree.

--*/
#include "precomp.h"
#include "ClusterCommand.h"
#include "DeveloperClusterCommand.h"

using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<std::unique_ptr<Command>> ClusterCommand::CreateCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"create", FullName(), DeveloperClusterOperation::Create));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(
        L"delete", FullName(), DeveloperClusterOperation::Delete, std::vector<std::wstring_view>{L"remove", L"rm"}));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"status", FullName(), DeveloperClusterOperation::Status));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"kubeconfig", FullName(), DeveloperClusterOperation::Kubeconfig));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"diagnostics", FullName(), DeveloperClusterOperation::Diagnostics));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"versions", FullName(), DeveloperClusterOperation::Versions));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"distributions", FullName(), DeveloperClusterOperation::Distributions));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"cnis", FullName(), DeveloperClusterOperation::Cnis));
    return commands;
}

std::wstring ClusterCommand::ShortDescription() const
{
    return Localization::WSLCCLI_DeveloperClusterCommandDesc();
}

std::wstring ClusterCommand::LongDescription() const
{
    return Localization::WSLCCLI_DeveloperClusterDesktopCommandLongDesc();
}

void ClusterCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

} // namespace wsl::windows::wslc
