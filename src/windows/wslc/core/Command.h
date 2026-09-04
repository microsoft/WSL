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
#include "ArgMap.h"
#include "CLIExecutionContext.h"
#include "Invocation.h"
#include "ArgumentParser.h"
#include "Terminal.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc {

enum class HelpOutput
{
    Full,
    Command,
    Argument,
};

// The executable name shown in usage/help output, set from argv[0] at startup.
extern std::wstring s_ExecutableName;

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
    virtual std::vector<ArgType> GetUnsupportedArguments() const
    {
        return {};
    }
    virtual std::vector<ArgumentDeprecation> GetArgumentDeprecations() const
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

    void OutputHelp(
        Terminal& terminal,
        HelpOutput output = HelpOutput::Full,
        const CommandException* exception = nullptr,
        std::span<const Argument> relevantArguments = {}) const;

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
        const std::vector<Argument>& overridableDefaults = {},
        std::vector<ArgumentDeprecation> deprecations = {},
        std::vector<ArgType> unsupportedArguments = {}) const;

    void ParseArguments(Invocation& inv, ArgMap& target) const
    {
        ParseArguments(inv, target, GetAllArguments(), false, false, {}, GetArgumentDeprecations(), GetUnsupportedArguments());
    }

    void ValidateArguments(ArgMap& source, const std::vector<Argument>& definedArgs, bool runInternalHook) const;

    void ValidateArguments(ArgMap& source) const
    {
        ValidateArguments(source, GetAllArguments(), true);
    }

    void OutputDeprecatedArgumentWarnings(Terminal& terminal, const ArgMap& source) const;

    virtual void Execute(CLIExecutionContext& context) const;

protected:
    // Command-specific validation hook, run after the shared per-argument Argument::Validate pass.
    // Override to enforce cross-argument rules that per-argument validation cannot express, such as
    // mutually-exclusive arguments or required argument combinations.
    //
    // Contract: this hook enforces relationships between already-validated arguments. It receives a
    // GetValue/GetAllValues make the selected argument immutable after returning it. Converted
    // arguments are validated on demand if needed.
    virtual void ValidateArgumentsInternal(ArgMap& source) const;
    virtual void ExecuteInternal(CLIExecutionContext& context) const = 0;

    std::vector<Argument> GetArgumentsForHelp(std::initializer_list<ArgType> types) const;

private:
    std::wstring_view m_name;
    std::vector<std::wstring_view> m_aliases;
    std::wstring m_fullName;
};

void Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command);
} // namespace wsl::windows::wslc
