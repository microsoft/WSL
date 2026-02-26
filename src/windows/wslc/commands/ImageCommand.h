/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Image Command
struct ImageCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"image";
    ImageCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct ImageListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    ImageListCommand(const std::wstring& parent) : Command(CommandName, {L"ls"}, parent)
    {
    }

    // Image list has an alias 'images' off the root, which will collide with the
    // container list command and its alias off the root. To avoid this, we will use
    // an override constructor that changes the name and alias of the command for when
    // it is parented directly to the root.
    ImageListCommand(const std::wstring& parent, const std::wstring_view name) : Command(name, {}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Pull Command
struct ImagePullCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"pull";
    ImagePullCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc