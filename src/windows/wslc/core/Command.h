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
#include "defs.h"

#include <functional>
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

    NON_COPYABLE(Command);
    NON_MOVABLE(Command);

    std::wstring_view Name() const
    {
        return m_name;
    }
    const std::wstring& FullName() const
    {
        return m_fullName;
    }
    std::wstring FormatInvocation(std::wstring_view name) const;
    std::wstring FormatInvocation() const
    {
        return FormatInvocation(Name());
    }
    const std::vector<std::wstring_view>& Aliases() const
    {
        return m_aliases;
    }

    const Command& Root() const;
    std::optional<std::reference_wrapper<const Command>> Parent() const noexcept
    {
        return m_parent;
    }

    const std::vector<std::unique_ptr<Command>>& GetCommands() const;

    virtual std::vector<Argument> GetArguments() const
    {
        return {};
    }

    virtual std::vector<Argument> GetGlobalArguments() const
    {
        return {};
    }

    // Flags::All returns every argument in the scope. Flags::None returns unflagged
    // arguments. Other values return arguments containing all requested flags.
    std::vector<Argument> GetScopedArguments(Scope scope, Flags flags = Flags::All) const;

    virtual std::wstring ShortDescription() const = 0;
    virtual std::wstring LongDescription() const = 0;

    void OutputHelp(
        Terminal& terminal,
        HelpOutput output = HelpOutput::Full,
        const CommandException* exception = nullptr,
        std::span<const Argument> relevantArguments = {}) const;

    std::optional<std::reference_wrapper<const Command>> FindSubCommand(InvocationCursor& invocation) const;

    // optionsOnly:          stop before the first positional token.
    // stopOnUnknown:        stop before the first unknown option
    //                       token instead of throwing. Note: applies per-token; a
    //                       bundled short chain (e.g. "-Dv") whose leading alias
    //                       is recognized is treated as claimed, and an unknown
    //                       alias later in the chain still throws.
    void ParseArguments(InvocationCursor& invocation, ArgMap& target, std::vector<Argument> definedArgs, bool optionsOnly = false, bool stopOnUnknown = false) const;

    void ParseArguments(InvocationCursor& invocation, ArgMap& target) const
    {
        ParseArguments(invocation, target, GetScopedArguments(Scope::Command, Flags::None));
    }

    void ValidateArguments(ArgMap& source, const std::vector<Argument>& definedArgs, bool runInternalHook) const;

    void ValidateArguments(ArgMap& source) const
    {
        ValidateArguments(source, GetScopedArguments(Scope::Command), true);
    }

    virtual void Execute(CLIExecutionContext& context) const;

protected:
    Argument CreateGlobalArgument(ArgType type, ArgumentOverrides overrides = {}) const;

    virtual std::vector<std::unique_ptr<Command>> CreateCommands() const
    {
        return {};
    }

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
    std::optional<std::reference_wrapper<const Command>> m_parent;
    mutable std::optional<std::vector<std::unique_ptr<Command>>> m_commands;
};
} // namespace wsl::windows::wslc
