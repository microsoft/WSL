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
// The argument parsing state machine.
// It is broken out to enable completion to process arguments, ignore errors,
// and determine the likely state of the word to be completed.
struct ParseArgumentsStateMachine
{
    ParseArgumentsStateMachine(Invocation& inv, ArgMap& execArgs, std::vector<Argument> arguments);

    ParseArgumentsStateMachine(const ParseArgumentsStateMachine&) = delete;
    ParseArgumentsStateMachine& operator=(const ParseArgumentsStateMachine&) = delete;

    ParseArgumentsStateMachine(ParseArgumentsStateMachine&&) = default;
    ParseArgumentsStateMachine& operator=(ParseArgumentsStateMachine&&) = default;

    // Processes the next argument from the invocation.
    // Returns true if there was an argument to process;
    // returns false if there were none.
    bool Step();

    // Throws if there was an error during the prior step.
    void ThrowIfError() const;

    // The current state of the state machine.
    // An empty state indicates that the next argument can be anything.
    struct State
    {
        State() = default;
        State(ArgType type, std::wstring_view arg) : m_type(type), m_arg(arg)
        {
        }
        State(ArgumentException ce) : m_exception(std::move(ce))
        {
        }

        // If set, indicates that the next argument is a value for this type.
        const std::optional<ArgType>& Type() const
        {
            return m_type;
        }

        // The actual argument string associated with Type.
        const std::wstring& Arg() const
        {
            return m_arg;
        }

        // If set, indicates that the last argument produced an error.
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

    // Gets the next positional argument, or nullptr if there is not one.
    const Argument* NextPositional();

    const std::vector<Argument>& Arguments() const
    {
        return m_arguments;
    }

private:
    State StepInternal();
    State ProcessFirstPositionalArgument(const std::wstring_view& currArg);
    State ProcessRemainingPositionals(const std::wstring_view& currArg);
    State ProcessAliasArgument(const std::wstring_view& currArg);
    State ProcessNamedArgument(const std::wstring_view& currArg);
    void ProcessAdjoinedValue(ArgType type, std::wstring_view value);
    void EscapeAndQuoteForwardedArgument(std::wstring& arg);

    Invocation& m_invocation;
    ArgMap& m_executionArgs;
    std::vector<Argument> m_arguments;

    Invocation::iterator m_invocationItr;
    std::vector<Argument>::iterator m_positionalSearchItr;

    // The anchor positional is the first positional argument processed.
    std::optional<Argument> m_anchorPositional = std::nullopt;

    // Separate arguments by Kind
    std::vector<Argument> m_standardArgs = {};
    std::vector<Argument> m_positionalArgs = {};
    std::vector<Argument> m_forwardArgs = {};

    State m_state;
};
} // namespace wsl::windows::wslc