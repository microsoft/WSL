/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Command.h

Abstract:

    Declaration of command class.

--*/
#pragma once
#include "Argument.h"
#include "Exceptions.h"
#include "ArgumentTypes.h"
#include "CLIExecutionContext.h"
#include "Invocation.h"
#include "ArgumentParser.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc {

constexpr std::wstring_view s_ExecutableName = L"wslc";

struct Command
{
    // The character used to split between commands and their parents in FullName.
    constexpr static wchar_t ParentSplitChar = L':';

    Command(std::wstring_view name, const std::wstring& parent) : Command(name, {}, parent)
    {
    }
    Command(std::wstring_view name, std::vector<std::wstring_view>&& aliases, const std::wstring& parent);

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
    const std::vector<std::wstring_view>& Aliases() const
    {
        return m_aliases;
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

    // Options accepted before any subcommand on the command line.
    virtual std::vector<Argument> GetGlobalArguments() const
    {
        return {};
    }

    // Args eligible for environment binding.
    virtual std::vector<Argument> GetEnvArguments() const
    {
        return {};
    }

    // Union of GetGlobalArguments() and GetEnvArguments(), deduped by ArgType
    // (globals win on conflict). Use this anywhere the two sets are combined
    // so duplicates are not parsed/validated twice.
    std::vector<Argument> GetGlobalsAndEnvArguments() const;

    virtual std::wstring ShortDescription() const = 0;
    virtual std::wstring LongDescription() const = 0;

    void OutputIntroHeader() const;
    void OutputHelp(const CommandException* exception = nullptr) const;

    std::unique_ptr<Command> FindSubCommand(Invocation& inv) const;

    // optionsOnly:          stop (without consuming) at the first positional token.
    // stopOnUnknown:        stop (without consuming) at the first unknown option
    //                       token instead of throwing. Note: applies per-token; a
    //                       bundled short chain (e.g. "-Dv") whose leading alias
    //                       is recognized is treated as claimed, and an unknown
    //                       alias later in the chain still throws.
    // overridableDefaults:  args whose preloaded entries in target are treated
    //                       as defaults (e.g. env-applied) and may be replaced
    //                       by the first CLI occurrence.
    void ParseArguments(
        Invocation& inv,
        ArgMap& target,
        std::vector<Argument> definedArgs,
        bool optionsOnly = false,
        bool stopOnUnknown = false,
        const std::vector<Argument>& overridableDefaults = {}) const;

    void ParseArguments(Invocation& inv, ArgMap& target) const
    {
        ParseArguments(inv, target, GetAllArguments());
    }

    void ValidateArguments(const ArgMap& source, const std::vector<Argument>& definedArgs, bool runInternalHook) const;

    void ValidateArguments(const ArgMap& source) const
    {
        ValidateArguments(source, GetAllArguments(), true);
    }

    virtual void Execute(CLIExecutionContext& context) const;

protected:
    virtual void ValidateArgumentsInternal(const ArgMap& source) const;
    virtual void ExecuteInternal(CLIExecutionContext& context) const = 0;

private:
    std::wstring_view m_name;
    std::vector<std::wstring_view> m_aliases;
    std::wstring m_fullName;
};

void Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command);
} // namespace wsl::windows::wslc
