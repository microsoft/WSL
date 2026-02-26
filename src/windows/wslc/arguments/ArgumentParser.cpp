/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentParser.cpp

Abstract:

    Implementation of the ArgumentParser class.

--*/
#include "ArgumentParser.h"
#include "Localization.h"

using namespace wsl::shared;

namespace wsl::windows::wslc {
ParseArgumentsStateMachine::ParseArgumentsStateMachine(Invocation& inv, ArgMap& execArgs, std::vector<Argument> arguments) :
    m_invocation(inv), m_executionArgs(execArgs), m_arguments(std::move(arguments)), m_invocationItr(m_invocation.begin())
{
    // Create sublists by Kind for easier processing in the state machine.
    for (const auto& arg : m_arguments)
    {
        switch (arg.Kind())
        {
        case Kind::Value:
            m_standardArgs.emplace_back(arg);
            break;
        case Kind::Flag:
            m_standardArgs.emplace_back(arg);
            break;
        case Kind::Positional:
            m_positionalArgs.emplace_back(arg);
            break;
        case Kind::Forward:
            m_forwardArgs.emplace_back(arg);
            break;
        }
    }

    m_positionalSearchItr = m_positionalArgs.begin();
}

bool ParseArgumentsStateMachine::Step()
{
    if (m_invocationItr == m_invocation.end())
    {
        return false;
    }

    m_state = StepInternal();
    return true;
}

void ParseArgumentsStateMachine::ThrowIfError() const
{
    if (m_state.Exception())
    {
        throw m_state.Exception().value();
    }
    // If the next argument was to be a value, but none was provided, convert it to an exception.
    else if (m_state.Type() && m_invocationItr == m_invocation.end())
    {
        throw ArgumentException(Localization::WSLCCLI_MissingArgumentError(m_state.Arg()));
    }
}

const Argument* ParseArgumentsStateMachine::NextPositional()
{
    // Find the next appropriate positional arg if the current itr isn't one or has hit its limit.
    while (m_positionalSearchItr != m_positionalArgs.end() &&
           (m_executionArgs.Count(m_positionalSearchItr->Type()) == m_positionalSearchItr->Limit()))
    {
        ++m_positionalSearchItr;
    }

    if (m_positionalSearchItr == m_positionalArgs.end())
    {
        return nullptr;
    }

    return &*m_positionalSearchItr;
}

// Parse arguments as such:
//  1. If argument starts with a single -, the alias is considered (can be 1-2 characters).
//      a. If the named argument alias (a or ab) needs a VALUE, it can be provided in these ways:
//          -a=VALUE or -ab=VALUE
//          -a VALUE or -ab VALUE
//      b. If the argument is a flag, additional characters after are treated as if they start
//          with a -, repeatedly until the end of the argument is reached.  Fails if non-flags hit.
//  2. If the argument starts with a double --, only the full name is considered.
//      a. If the named argument (arg) needs a VALUE, it can be provided in these ways:
//          --arg=VALUE
//          --arg VALUE
//  3. If the argument does not start with any -, it is considered the next positional argument.
//  4. Once a positional argument is encountered, all subsequent arguments are considered positional
//  5. If the command only has 1 positional argument, all subsequent arguments are considered forwarded.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::StepInternal()
{
    // Get the next argument from the invocation.
    auto currArg = std::wstring_view{*m_invocationItr};
    ++m_invocationItr;

    // If current state has a type, then that means this must be a value for the previous argument.
    if (m_state.Type())
    {
        m_executionArgs.Add(m_state.Type().value(), std::wstring{currArg});
        return {};
    }

    // If this command has forwarded args present and we have found a positional argument,
    // the all remaining args are considered positional or forwarded.
    if (!m_forwardArgs.empty() && m_anchorPositional.has_value())
    {
        return ProcessAnchoredPositionals(currArg);
    }

    // Arg does not begin with '-' so it is neither an alias nor a named value, must be positional.
    if (currArg.empty() || currArg[0] != WSLC_CLI_ARG_ID_CHAR)
    {
        return ProcessPositionalArgument(currArg);
    }

    // The currentArg is non-empty, and starts with a -.
    if (currArg.length() == 1)
    {
        // If it is only one character, then it is an error since it is neither an alias nor a named argument.
        return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
    }

    // Single '-' that is 2 characters or more means this must be an alias or collection of alias flags.
    if (currArg[1] != WSLC_CLI_ARG_ID_CHAR)
    {
        return ProcessAliasArgument(currArg);
    }

    // The currentArg must be a named argument.
    return ProcessNamedArgument(currArg);
}

// Assumes non-empty and does not begin with '-'.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessPositionalArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(!currArg.empty() && currArg[0] != WSLC_CLI_ARG_ID_CHAR);

    const Argument* nextPositional = NextPositional();
    if (!nextPositional)
    {
        return ArgumentException(Localization::WSLCCLI_ExtraPositionalError(currArg));
    }

    // First positional found is the anchor positional.
    if (!m_anchorPositional.has_value())
    {
        m_anchorPositional = Argument(*nextPositional);
    }

    m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
    return {};
}

// Assumes one positional has already been found and therefore there are no remaining Kind Value/Flag arguments.
// Only Kind::Positional or Kind::Forward arguments should remain.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessAnchoredPositionals(const std::wstring_view& currArg)
{
    WI_ASSERT(m_anchorPositional.has_value());

    // If we haven't reached the limit for the anchor positional, treat this as another anchor positional.
    // Anchors with NO_LIMIT will never be full and therefore will always treat subsequent positionals as anchors.
    if ((m_executionArgs.Count(m_anchorPositional.value().Type()) < m_anchorPositional.value().Limit()) ||
        (m_anchorPositional.value().Limit() == NO_LIMIT))
    {
        // validate that we dont have any invalid argument specifiers.
        if (!currArg.empty() && currArg[0] == WSLC_CLI_ARG_ID_CHAR)
        {
            return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
        }

        m_executionArgs.Add(m_anchorPositional.value().Type(), std::wstring{currArg});
        return {};
    }

    // There are three possibilities for this argument:
    // 1) It is another positional argument (ex: run <imagename> <command>)
    // 2) It is a forwarded argument set that could be anything (most likely)
    // 3) It is an input error and there should be no such argument.

    // Check next positional.
    const Argument* nextPositional = NextPositional();
    if (nextPositional)
    {
        // validate that we dont have any invalid argument specifiers.
        if (!currArg.empty() && currArg[0] == WSLC_CLI_ARG_ID_CHAR)
        {
            return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
        }

        m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
        return {};
    }

    // Handle case where we expect a positional but

    // Check for forwarded arg existence.
    if (m_forwardArgs.empty())
    {
        return ArgumentException(Localization::WSLCCLI_CommandHasNoForwardArgumentsError(currArg));
    }

    // currArg is the first forwarded argument
    // All the rest of the args are forward args.
    std::vector<std::wstring> forwardedArgs;
    forwardedArgs.emplace_back(std::wstring{currArg});
    while (m_invocationItr != m_invocation.end())
    {
        forwardedArgs.emplace_back(std::wstring{*m_invocationItr});
        ++m_invocationItr;
    }

    m_executionArgs.Add(m_forwardArgs.front().Type(), std::move(forwardedArgs));
    return {};
}

// Assumes argument begins with '-' and is at least 2 characters.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessAliasArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(currArg.length() >= 2 && currArg[0] == WSLC_CLI_ARG_ID_CHAR && currArg[1] != WSLC_CLI_ARG_ID_CHAR);

    // This may be a collection of boolean alias flags.
    // Helper to find an argument by alias starting at a specific position.
    auto findArgumentByAlias = [this](const std::wstring_view& str, size_t startPos, size_t& aliasLength) -> const Argument* {
        for (const auto& arg : m_standardArgs)
        {
            const auto& alias = arg.Alias();
            if (alias.empty())
            {
                continue;
            }

            if (startPos + alias.length() <= str.length() && str.compare(startPos, alias.length(), alias) == 0)
            {
                aliasLength = alias.length();
                return &arg;
            }
        }

        return nullptr;
    };

    // Find the first alias starting at position 1 (after the '-')
    size_t aliasLength = 0;
    const Argument* firstArg = findArgumentByAlias(currArg, 1, aliasLength);
    if (!firstArg)
    {
        return ArgumentException(Localization::WSLCCLI_InvalidAliasError(currArg));
    }

    // Position after the first alias
    size_t currentPos = 1 + aliasLength;

    // Check if this argument expects a value
    if (firstArg->Kind() == Kind::Value)
    {
        // Kind::Value is only allowed if it's the last flag (no more characters after it, or '=' follows)
        if (currentPos >= currArg.length())
        {
            // No more characters - value should be in next argument
            return {firstArg->Type(), currArg};
        }

        if (currArg[currentPos] != WSLC_CLI_ARG_SPLIT_CHAR)
        {
            // There are more characters but it's not '=' - this is invalid
            return ArgumentException(Localization::WSLCCLI_ValueMustBeLastInAliasChainError(currArg));
        }

        // Value is adjoined after '='
        ProcessAdjoinedValue(firstArg->Type(), currArg.substr(currentPos + 1));
        return {};
    }

    // Boolean flag - add it and process any adjoined flags
    m_executionArgs.Add(firstArg->Type(), true);

    // Process remaining adjoined flags
    while (currentPos < currArg.length())
    {
        const Argument* nextArg = findArgumentByAlias(currArg, currentPos, aliasLength);

        if (!nextArg)
        {
            return ArgumentException(Localization::WSLCCLI_AdjoinedNotFoundError(currArg));
        }

        // Update position before checking Kind
        size_t nextPos = currentPos + aliasLength;

        if (nextArg->Kind() == Kind::Value)
        {
            // Kind::Value is only allowed if it's the last flag
            if (nextPos >= currArg.length())
            {
                // No more characters - value should be in next argument
                return {nextArg->Type(), currArg};
            }

            if (currArg[nextPos] != WSLC_CLI_ARG_SPLIT_CHAR)
            {
                // There are more characters but it's not '=' - this is invalid
                return ArgumentException(Localization::WSLCCLI_ValueMustBeLastInAliasChainError(currArg));
            }

            // Value is adjoined after '='
            ProcessAdjoinedValue(nextArg->Type(), currArg.substr(nextPos + 1));
            return {};
        }

        m_executionArgs.Add(nextArg->Type(), true);
        currentPos = nextPos;
    }

    return {};
}

// Assumes the arg value begins with -- and is at least 2 characters long.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessNamedArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(currArg.starts_with(L"--"));

    if (currArg.length() == 2)
    {
        // Missing argument name after double dash, this is an error.
        return ArgumentException(Localization::WSLCCLI_MissingArgumentNameError(currArg));
    }

    // This is an arg name, find it and process its value if needed.
    // Skip the double arg identifier chars.
    size_t argStart = currArg.find_first_not_of(WSLC_CLI_ARG_ID_CHAR);
    std::wstring_view argName = currArg.substr(argStart);
    bool argFound = false;

    bool hasAdjoinedValue = false;
    std::wstring_view argValue;
    size_t splitChar = argName.find_first_of(WSLC_CLI_ARG_SPLIT_CHAR);
    if (splitChar != std::string::npos)
    {
        // There is an '=' in this arg, it has an adjoined value, split it out.
        hasAdjoinedValue = true;
        argValue = argName.substr(splitChar + 1);
        argName = argName.substr(0, splitChar);
    }

    // Find a matching standard arg with this name.
    for (const auto& arg : m_standardArgs)
    {
        if (string::IsEqual(argName, arg.Name()))
        {
            // Found a match, process by kind.
            if (arg.Kind() == Kind::Flag)
            {
                // TODO: Consider supporting --flag and --flag=true or --flag=false for bool args.
                if (hasAdjoinedValue)
                {
                    return ArgumentException(Localization::WSLCCLI_FlagContainAdjoinedError(currArg));
                }

                m_executionArgs.Add(arg.Type(), true);
                return {};
            }

            // Not a Flag, must be a Value, and therefore must have a value provided.
            if (hasAdjoinedValue)
            {
                ProcessAdjoinedValue(arg.Type(), argValue);
                return {};
            }

            // The value should be the next argument.
            return {arg.Type(), currArg};
        }
    }

    // We found no matching argument for this name, this is an invalid argument name.
    return ArgumentException(Localization::WSLCCLI_InvalidNameError(currArg));
}

void ParseArgumentsStateMachine::ProcessAdjoinedValue(ArgType type, std::wstring_view value)
{
    // If the adjoined value is wrapped in quotes, strip them off.
    if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
    {
        value = value.substr(1, value.length() - 2);
    }

    m_executionArgs.Add(type, std::wstring{value});
}
} // namespace wsl::windows::wslc
