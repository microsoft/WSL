/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeveloperClusterCommand.h

Abstract:

    Standalone developer cluster helpers used by the cluster command tree.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {

enum class DeveloperClusterOperation
{
    Create,
    Delete,
    Status,
    Kubeconfig,
    Diagnostics,
    Versions,
    Distributions,
    Cnis,
};

std::vector<Argument> GetDeveloperClusterArguments(DeveloperClusterOperation operation);
void ValidateDeveloperClusterArguments(DeveloperClusterOperation operation, ArgMap& args);
void ExecuteDeveloperCluster(DeveloperClusterOperation operation, CLIExecutionContext& context);
std::wstring DeveloperClusterShortDescription(DeveloperClusterOperation operation);
std::wstring DeveloperClusterLongDescription(DeveloperClusterOperation operation);

struct DeveloperClusterActionCommand final : public Command
{
    DeveloperClusterActionCommand(
        std::wstring_view name, const std::wstring& parent, DeveloperClusterOperation operation, std::vector<std::wstring_view>&& aliases = {}) :
        Command(name, std::move(aliases), parent), m_operation(operation)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;

private:
    DeveloperClusterOperation m_operation;
};

} // namespace wsl::windows::wslc
