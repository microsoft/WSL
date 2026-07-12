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
    //                       clears the preexisting entry first, so a single-value
    //                       arg can be overridden on the command line even though
    //                       Limit() == 1. Subsequent Adds in the same parse run
    //                       behave normally and still enforce Limit, so
    //                       duplicates on the command line itself are caught.
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

    // Routes a flag add through the override/idempotency rules:
    //  - if type is in m_overridableDefaults, the preloaded value is replaced;
    //  - else if the flag is already set, the add is a no-op (CLI duplicates
    //    fold to a single entry, matching docker / kubectl / git style).
    void AddFlag(ArgType type);

    // Routes a value add through the override rule. CLI duplicates of value
    // args still stack, so Validate() will catch exceeding Limit.
    void AddValue(ArgType type, std::wstring value);

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
