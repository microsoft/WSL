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
#include "ArgumentTypes.h"

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
    // optionsOnly:          stop (without consuming) at the first positional token.
    // stopOnUnknown:        stop (without consuming) at the first unknown option
    //                       token instead of throwing.
    // overridableDefaults:  ArgTypes whose existing entries in execArgs are
    //                       treated as preloaded defaults (e.g. from environment
    //                       variables). The first CLI Add for one of these types
    //                       clears the preexisting entry first, so a preloaded
    //                       default is replaced rather than appended to. Single-value
    //                       args are last-wins regardless, so a later CLI duplicate
    //                       simply overwrites; unlimited args accumulate once the
    //                       preloaded default has been dropped.
    ParseArgumentsStateMachine(
        Invocation& inv,
        ArgMap& execArgs,
        std::vector<Argument> arguments,
        bool optionsOnly = false,
        bool stopOnUnknown = false,
        const std::vector<Argument>& overridableDefaults = {});

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

    // In optionsOnly / stopOnUnknown modes this points at the first unconsumed token.
    Invocation::iterator Position() const
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

    void AdvanceToNextPositional(std::vector<Argument>::iterator& itr) const;

    // Backs up one token and stops cleanly so Position() points at the unconsumed token.
    State BackUpAndStop();

    // Sets a boolean flag using presence to encode its value: a true value stores a
    // single entry, a false value clears the flag entirely. Keeping "present == true"
    // lets the whole CLI test flags with Contains(), and gives docker-style last-wins
    // semantics for repeats (e.g. "--flag --flag=false" ends up false, and duplicate
    // "--flag --flag" folds to a single entry).
    void SetFlag(ArgType type, bool value);

    // Parses an adjoined boolean token for a flag (e.g. the "false" in "--flag=false" or
    // "-f=false"; accepts true/false/1/0, case-insensitive) and applies it via SetFlag.
    // Returns an error State if the token is not a recognized boolean. Shared by the
    // alias, alias-chain, and named-flag paths so all three treat "=value" identically.
    State ApplyFlagValue(ArgType type, std::wstring_view value, const std::wstring_view& currArg);

    // Removes all entries for an argument and consumes any overridable-default slot,
    // leaving the argument absent. This is the single-value (last-wins) primitive that
    // SetFlag builds on; it is written to be reused for other single-value argument
    // kinds in the future.
    void ClearArgument(ArgType type);

    // Stores a value for a Kind::Value argument. Single-value args are last-wins
    // (any previous value, including a preloaded overridable default, is cleared
    // first); unlimited args accumulate but still let the first CLI value replace a
    // preloaded default.
    void AddValue(ArgType type, std::wstring value);

    // Returns the defined argument for a type, or nullptr if it is not one of this
    // parser's arguments. Used to consult an argument's Limit while parsing values.
    const Argument* FindArgument(ArgType type) const;

    // If type is in m_overridableDefaults, removes any existing entry and
    // consumes the override slot. Returns true if an override was consumed.
    bool ConsumeOverrideIfPresent(ArgType type);

    Invocation& m_invocation;
    ArgMap& m_executionArgs;
    std::vector<Argument> m_arguments;

    Invocation::iterator m_invocationItr;
    std::vector<Argument>::iterator m_positionalSearchItr;

    // First positional processed; anchors handling of subsequent positionals/forwards.
    std::optional<Argument> m_anchorPositional = std::nullopt;

    std::vector<Argument> m_standardArgs = {};
    std::vector<Argument> m_positionalArgs = {};
    std::vector<Argument> m_forwardArgs = {};

    State m_state;

    // When true, stop cleanly at the first positional token (do not consume it).
    bool m_optionsOnly = false;

    // When true, stop cleanly (do not consume) at the first unknown option token.
    bool m_stopOnUnknown = false;

    // Set when m_optionsOnly or m_stopOnUnknown stopped processing.
    bool m_stopped = false;

    // ArgTypes whose preloaded value should be replaced by the first CLI add.
    // Empties as overrides are consumed so a single preload can only be
    // overridden once per parse.
    std::vector<ArgType> m_overridableDefaults;
};
} // namespace wsl::windows::wslc
