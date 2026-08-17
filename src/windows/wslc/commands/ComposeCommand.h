/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeCommand.h

Abstract:

    Declares the minimal compose command tree.

--*/

#pragma once

#include "Command.h"

namespace wsl::windows::wslc {

struct ComposeCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"compose";
    ComposeCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

#define DECLARE_COMPOSE_COMMAND(TypeName, NameValue) \
    struct TypeName final : public Command \
    { \
        constexpr static std::wstring_view CommandName = NameValue; \
        TypeName(const std::wstring& Parent) : Command(CommandName, Parent) \
        { \
        } \
        std::vector<Argument> GetArguments() const override; \
        std::wstring ShortDescription() const override; \
        std::wstring LongDescription() const override; \
\
    protected: \
        void ExecuteInternal(CLIExecutionContext& Context) const override; \
    }

DECLARE_COMPOSE_COMMAND(ComposeCreateCommand, L"create");
DECLARE_COMPOSE_COMMAND(ComposeStartCommand, L"start");
DECLARE_COMPOSE_COMMAND(ComposeAttachCommand, L"attach");
DECLARE_COMPOSE_COMMAND(ComposeStopCommand, L"stop");

#undef DECLARE_COMPOSE_COMMAND

} // namespace wsl::windows::wslc
