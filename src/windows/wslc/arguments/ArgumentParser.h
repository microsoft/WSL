/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentParser.h

Abstract:

    Declaration of the ArgumentParser class for command-line argument parsing.

--*/
#pragma once
#include "Argument.h"
#include "Exceptions.h"
#include "Invocation.h"
#include "ArgMap.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <type_traits>

namespace wsl::windows::wslc {
// State machine is exposed so completion can run the parser, ignore errors,
// and inspect the in-progress state of the word being completed.
struct ParseArgumentsStateMachine
{
    // optionsOnly:          stop before the first positional token.
    // stopOnUnknown:        stop before the first unknown option
    //                       token instead of throwing.
    ParseArgumentsStateMachine(InvocationCursor& invocation, ArgMap& execArgs, std::vector<Argument> arguments, bool optionsOnly = false, bool stopOnUnknown = false);

    ParseArgumentsStateMachine(const ParseArgumentsStateMachine&) = delete;
    ParseArgumentsStateMachine& operator=(const ParseArgumentsStateMachine&) = delete;

    ParseArgumentsStateMachine(ParseArgumentsStateMachine&&) = default;
    ParseArgumentsStateMachine& operator=(ParseArgumentsStateMachine&&) = default;

    // Returns false when there is nothing left to process.
    bool Step();

    void ThrowIfError() const;

    // Empty state means the next argument can be anything.
    struct State
    {
        State() = default;
        State(ArgType type, std::wstring_view arg) : m_type(type), m_arg(arg)
        {
        }
        State(ArgumentException ce) : m_exception(std::move(ce))
        {
        }

        // If set, the next argument is a value for this type.
        const std::optional<ArgType>& Type() const
        {
            return m_type;
        }

        const std::wstring& Arg() const
        {
            return m_arg;
        }

        const std::optional<ArgumentException>& Exception() const
        {
            return m_exception;
        }

    private:
        std::optional<ArgType> m_type;
        std::wstring m_arg;
        std::optional<ArgumentException> m_exception;
    };

    const State& GetState() const
    {
        return m_state;
    }

    const Argument* NextPositional();

    // Non-advancing variant of NextPositional.
    bool HasNextPositional() const;

    const std::vector<Argument>& Arguments() const
    {
        return m_arguments;
    }

    // In optionsOnly / stopOnUnknown modes this points at the next token.
    InvocationCursor::iterator Position() const
    {
        return m_invocationItr;
    }

private:
    State StepInternal();
    State ProcessPositionalArgument(const std::wstring_view& currArg);
    State ProcessAnchoredPositionals(const std::wstring_view& currArg);
    State ProcessAliasArgument(const std::wstring_view& currArg);
    State ProcessNamedArgument(const std::wstring_view& currArg);
    void ProcessAdjoinedValue(ArgType type, std::wstring_view value);

    // Strips a single pair of surrounding double quotes from an adjoined value if present
    // (e.g. --name="value" or --flag="true"). Shared by the value and flag adjoined-value
    // paths so both treat quoted "=value" tokens identically.
    static std::wstring_view StripSurroundingQuotes(std::wstring_view value);

    void AdvanceToNextPositional(std::vector<Argument>::iterator& itr) const;

    // Backs up one token and stops cleanly so Position() points at the next token.
    State BackUpAndStop();

    // Sets a boolean flag by storing its explicit parsed value (true or false). Clearing first
    // collapses CLI duplicates to a single entry, so a repeated flag is docker-style last-wins
    // (e.g. "--flag --flag=false" ends up false) and a duplicate "--flag --flag" folds to one
    // entry. Consumers read the flag with ArgMap::GetValue(defaultValue),
    // which lets a flag default to on and be disabled with "--flag=false".
    void SetFlag(ArgType type, bool value);

    // Parses an adjoined boolean token for a flag (e.g. the "false" in "--flag=false" or
    // "-f=false"). A single pair of surrounding double quotes is stripped first (so
    // "--flag=\"true\"" works like the value path), then the token is parsed as a Docker-style
    // boolean (true/false/1/0/t/f, case-insensitive) and applied via SetFlag. Returns an error
    // State if the token is not a recognized boolean. Shared by the alias, alias-chain, and
    // named-flag paths so all three treat "=value" identically.
    State ApplyFlagValue(ArgType type, std::wstring_view value, const std::wstring_view& currArg);

    // Removes all entries for an argument. This is the single-value (last-wins)
    // primitive that SetFlag builds on.
    void ClearArgument(ArgType type);

    // Stores a value for a Kind::Value argument. Single-value args are last-wins
    // and unlimited args accumulate.
    void AddValue(ArgType type, std::wstring value);

    // Returns the defined argument for a type, or nullptr if it is not one of this
    // parser's arguments. Used to consult an argument's Limit while parsing values.
    const Argument* FindArgument(ArgType type) const;

    InvocationCursor& m_invocation;
    ArgMap& m_executionArgs;
    std::vector<Argument> m_arguments;

    InvocationCursor::iterator m_invocationItr;
    std::vector<Argument>::iterator m_positionalSearchItr;

    // First positional processed; anchors handling of subsequent positionals/forwards.
    std::optional<Argument> m_anchorPositional = std::nullopt;

    std::vector<Argument> m_standardArgs = {};
    std::vector<Argument> m_positionalArgs = {};
    std::vector<Argument> m_forwardArgs = {};

    State m_state;

    // When true, stop before the first positional token.
    bool m_optionsOnly = false;

    // When true, stop before the first unknown option token.
    bool m_stopOnUnknown = false;

    // Set when m_optionsOnly or m_stopOnUnknown stopped processing.
    bool m_stopped = false;
};
} // namespace wsl::windows::wslc
