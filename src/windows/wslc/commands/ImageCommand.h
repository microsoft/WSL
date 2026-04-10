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

// Build Command
struct ImageBuildCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"build";
    ImageBuildCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct ImageListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";

    // When parented directly to the root, ImageListCommand uses a different name
    // to avoid colliding with the container list command.
    constexpr static std::wstring_view RootCommandName = L"images";

    ImageListCommand(const std::wstring& parent) : Command(CommandName, {L"ls"}, parent)
    {
    }

    // Image list has an alias 'images' off the root, which will collide with the
    // container list command and its alias off the root. To avoid this, we will use
    // an override constructor that changes the name and alias of the command for when
    // it is parented directly to the root.
    // The bool parameter is used as a tag to select the root-specific name.
    ImageListCommand(const std::wstring& parent, bool /*rootScoped*/) : Command(RootCommandName, {}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Load Command
struct ImageLoadCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"load";
    ImageLoadCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Remove Command
struct ImageRemoveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"remove";

    // When parented directly to the root, ImageRemoveCommand uses a different name
    constexpr static std::wstring_view RootCommandName = L"rmi";

    ImageRemoveCommand(const std::wstring& parent) : Command(CommandName, {L"delete", L"rm"}, parent)
    {
    }

    // Image remove has an alias 'rmi' off the root
    // The bool parameter is used as a tag to select the root-specific name.
    ImageRemoveCommand(const std::wstring& parent, bool /*rootScoped*/) : Command(RootCommandName, {}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Inspect Command
struct ImageInspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    ImageInspectCommand(const std::wstring& parent) : Command(CommandName, parent)
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

// Push Command
struct ImagePushCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"push";
    ImagePushCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Save Command
struct ImageSaveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"save";
    ImageSaveCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc