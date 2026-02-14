/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Command.h

Abstract:

    Declaration of command class.

--*/
#pragma once
#include "precomp.h"
#include "Argument.h"
#include "Exceptions.h"
#include "ArgumentTypes.h"
#include "CLIExecutionContext.h"
#include "Invocation.h"
#include "ArgumentParser.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc {
struct Command
{
    // The character used to split between commands and their parents in FullName.
    constexpr static wchar_t ParentSplitChar = L':';

    Command(std::wstring_view name, std::wstring parent);

    virtual ~Command() = default;

    Command(const Command&) = default;
    Command& operator=(const Command&) = default;

    Command(Command&&) = default;
    Command& operator=(Command&&) = default;

    std::wstring_view Name() const
    {
        return m_name;
    }
    const std::wstring& FullName() const
    {
        return m_fullName;
    }

    virtual std::vector<std::unique_ptr<Command>> GetCommands() const
    {
        return {};
    }
    virtual std::vector<Argument> GetArguments() const
    {
        return {};
    }

    virtual std::vector<Argument> GetAllArguments() const
    {
        auto args = GetArguments();
        args.emplace_back(Argument::Create(ArgType::Help));
        return args;
    }

    virtual std::wstring ShortDescription() const = 0;
    virtual std::wstring LongDescription() const = 0;

    void OutputIntroHeader() const;
    void OutputHelp(const CommandException* exception = nullptr) const;

    std::unique_ptr<Command> FindSubCommand(Invocation& inv) const;
    void ParseArguments(Invocation& inv, ArgMap& execArgs) const;
    void ValidateArguments(ArgMap& execArgs) const;

    virtual void Execute(CLIExecutionContext& context) const;

protected:
    virtual void ExecuteInternal(CLIExecutionContext& context) const;

private:
    std::wstring_view m_name;
    std::wstring m_fullName;
};

void Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command);
} // namespace wsl::windows::wslc