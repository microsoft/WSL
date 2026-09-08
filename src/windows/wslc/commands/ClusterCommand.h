/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ClusterCommand.h

Abstract:

    AKS Arc cluster command declarations.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {

struct ClusterCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"cluster";
    ClusterCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    ClusterCreateCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterDeleteCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"delete";
    ClusterDeleteCommand(const std::wstring& parent) : Command(CommandName, {L"remove", L"rm"}, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterStatusCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"status";
    ClusterStatusCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterKubeconfigCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"kubeconfig";
    ClusterKubeconfigCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterFleetCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"fleet";
    ClusterFleetCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterFleetJoinCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"join";
    ClusterFleetJoinCommand(const std::wstring& parent) : Command(CommandName, {L"attach"}, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(ArgMap& args) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterFleetLeaveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"leave";
    ClusterFleetLeaveCommand(const std::wstring& parent) : Command(CommandName, {L"detach"}, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ClusterFleetStatusCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"status";
    ClusterFleetStatusCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
