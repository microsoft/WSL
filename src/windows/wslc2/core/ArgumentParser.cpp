// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "ArgumentParser.h"
#include "Localization.h"

#include <algorithm>
#include <string>

using namespace wsl::shared;

namespace wsl::windows::wslc
{
    ParseArgumentsStateMachine::ParseArgumentsStateMachine(Invocation& inv, Args& execArgs, std::vector<Argument> arguments) :
        m_invocation(inv),
        m_executionArgs(execArgs),
        m_arguments(std::move(arguments)),
        m_invocationItr(m_invocation.begin()),
        m_positionalSearchItr(m_arguments.begin())
    {
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
        while (m_positionalSearchItr != m_arguments.end() && 
               ((m_positionalSearchItr->Kind() != Kind::Positional) || 
                (m_executionArgs.Count(m_positionalSearchItr->Type()) == m_positionalSearchItr->Limit())))
        {
            ++m_positionalSearchItr;
        }

        if (m_positionalSearchItr == m_arguments.end())
        {
            return nullptr;
        }

        return &*m_positionalSearchItr;
    }

    // Parse arguments as such:
    //  1. If argument starts with a single -, only the single character alias is considered.
    //      a. If the named argument alias (a) needs a VALUE, it can be provided in these ways:
    //          -a=VALUE
    //          -a VALUE
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
        auto currArg = std::wstring_view{ *m_invocationItr };
        ++m_invocationItr;

        // If the previous step indicated a value was needed, set it and forget it.
        if (m_state.Type())
        {
            m_executionArgs.Add(m_state.Type().value(), std::wstring{currArg});
            return {};
        }

        // If the anchor is found then all remaining args are considered positional or forwarded.
        if (m_anchorPositionalArgumentFound)
        {
            // This is either an additional positional argument for the anchor if
            // it has not yet reached its limit, otherwise it is a forwarded argument.

            // If we haven't reached the limit for the anchor positional, treat this as another anchor positional.
            if (m_executionArgs.Count(m_anchorPositional) < Argument::ForType(m_anchorPositional).Limit())
            {
                // validate that we dont have any invalid argument specifiers.
                if (!currArg.empty() && currArg[0] == WSLC_CLI_ARG_ID_CHAR)
                {
                    return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
                }

                m_executionArgs.Add(m_anchorPositional, std::wstring{currArg});
                return {};
            }

            // There are three possibilities for this argument:
            // 1) It is another positional argument (likely an unexpected scenario)
            // 2) It is a forwarded argument set that could be anything (most likely)
            // 3) It is an input error and there should be no such argument.

            // Check next positional.
            const Argument* nextPositional = NextPositional();
            if (nextPositional)
            {
                m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
                return {};
            }

            // TODO: Also check that forwarded arg kind was not used earlier. Forwarded args
            // should always be the last argument type encountered.
            // Check for forwarded arg existence.
            auto forwardedArgItr = std::find_if(m_arguments.begin(), m_arguments.end(),
                [](const Argument& arg) { return arg.Kind() == Kind::Forward; });

            if (forwardedArgItr != m_arguments.end())
            {
                // We have forwarded args type. Collect all remaining args as forwarded args.
                std::vector<std::wstring> forwardedArgs;

                // Add the current arg
                forwardedArgs.push_back(std::wstring{currArg});

                // Collect all remaining args from the invocation
                while (m_invocationItr != m_invocation.end())
                {
                    forwardedArgs.push_back(std::wstring{*m_invocationItr});
                    ++m_invocationItr;
                }

                // Add the vector to the forwarded ArgType.
                m_executionArgs.Add(forwardedArgItr->Type(), std::move(forwardedArgs));
                return {};
            }

            // At this point we have an extra positional argument
            return ArgumentException(Localization::WSLCCLI_ExtraPositionalError(currArg));
        }
        // This is the first positional argument we have encountered.
        else if (currArg.empty() || currArg[0] != WSLC_CLI_ARG_ID_CHAR)
        {
            const Argument* nextPositional = NextPositional();
            if (!nextPositional)
            {
                return ArgumentException(Localization::WSLCCLI_ExtraPositionalError(currArg));
            }

            // Anchor positional found. We treat all subsequent args as either additional
            // positional args for the anchor or forwarded args.
            m_anchorPositionalArgumentFound = true;
            m_anchorPositional = nextPositional->Type();
            m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
        }

        // The currentArg must not be empty, and starts with a -
        else if (currArg.length() == 1)
        {
            return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
        }
        // Now it must be at least 2 chars
        else if (currArg[1] != WSLC_CLI_ARG_ID_CHAR)
        {
            // Parse the single character alias argument
            auto currChar = currArg[1];

            auto itr = std::find_if(m_arguments.begin(), m_arguments.end(), [&](const Argument& arg) { return (currChar == arg.Alias()); });
            if (itr == m_arguments.end())
            {
                return ArgumentException(Localization::WSLCCLI_InvalidAliasError(currArg));
            }

            if (argument::Args::GetValueType(itr->Type()) == ValueType::Bool)
            {
                m_executionArgs.Add(itr->Type(), true);

                for (size_t i = 2; i < currArg.length(); ++i)
                {
                    currChar = currArg[i];

                    auto itr2 = std::find_if(m_arguments.begin(), m_arguments.end(), [&](const Argument& arg) { return (currChar == arg.Alias()); });
                    if (itr2 == m_arguments.end())
                    {
                        return ArgumentException(Localization::WSLCCLI_AdjoinedNotFoundError(currArg));
                    }
                    else if (argument::Args::GetValueType(itr2->Type()) != ValueType::Bool)
                    {
                        return ArgumentException(Localization::WSLCCLI_AdjoinedNotFlagError(currArg));
                    }
                    else
                    {
                        m_executionArgs.Add(itr2->Type(), true);
                    }
                }
            }
            else if (currArg.length() > 2)
            {
                if (currArg[2] == WSLC_CLI_ARG_SPLIT_CHAR)
                {
                    // Subtract the first 3 chars (-a=) to get the adjoined value.
                    ProcessAdjoinedValue(itr->Type(), currArg.substr(3));
                }
                else
                {
                    return ArgumentException(Localization::WSLCCLI_SingleCharAfterDashError(currArg));
                }
            }
            else
            {
                return {itr->Type(), currArg};
            }
        }
        // The currentArg is at least 2 chars, both of which are --
        else if (currArg.length() == 2)
        {
            // Missing argument name after double dash, this is an error.
            return ArgumentException(Localization::WSLCCLI_MissingArgumentNameError(currArg));
        }
        // The currentArg is more than 2 chars, both of which are --
        else
        {
            // This is an arg name, find it and process its value if needed.
            // Skip the double arg identifier chars.
            size_t argStart = currArg.find_first_not_of(WSLC_CLI_ARG_ID_CHAR);
            std::wstring_view argName = currArg.substr(argStart);
            bool argFound = false;

            bool hasValue = false;
            std::wstring_view argValue;
            size_t splitChar = argName.find_first_of(WSLC_CLI_ARG_SPLIT_CHAR);
            if (splitChar != std::string::npos)
            {
                hasValue = true;
                argValue = argName.substr(splitChar + 1);
                argName = argName.substr(0, splitChar);
            }

            for (const auto& arg : m_arguments)
            {
                if (string::IsEqual(argName, arg.Name()) ||
                    string::IsEqual(argName, arg.AlternateName()))
                {
                    if (argument::Args::GetValueType(arg.Type()) == ValueType::Bool)
                    {
                        // TODO: Consider supporting --flag and --flag=true or --flag=false for boolean args.
                        if (hasValue)
                        {
                            return ArgumentException(Localization::WSLCCLI_FlagContainAdjoinedError(currArg));
                        }

                        m_executionArgs.Add(arg.Type(), true);
                    }
                    else if (hasValue)
                    {
                        ProcessAdjoinedValue(arg.Type(), argValue);
                    }
                    else
                    {
                        return { arg.Type(), currArg };
                    }
                    argFound = true;
                    break;
                }
            }

            if (!argFound)
            {
                return ArgumentException(Localization::WSLCCLI_InvalidNameError(currArg));
            }
        }

        // If we get here, the next argument can be anything again.
        return {};
    }

    void ParseArgumentsStateMachine::ProcessAdjoinedValue(ArgType type, std::wstring_view value)
    {
        // If the adjoined value is wrapped in quotes, strip them off.
        if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
        {
            value = value.substr(1, value.length() - 2);
        }

        m_executionArgs.Add(type, std::wstring{ value });
    }
}