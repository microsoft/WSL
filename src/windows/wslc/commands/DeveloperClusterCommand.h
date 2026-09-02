/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeveloperClusterCommand.h

Abstract:

    Standalone developer cluster command declarations.

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

struct DeveloperClusterCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"developer-cluster";

    DeveloperClusterCommand(const std::wstring& parent) : Command(CommandName, {L"dev-cluster"}, parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

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
