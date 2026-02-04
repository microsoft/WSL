// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "argument.h"
#include "command.h"
#include "invocation.h"
#include "TaskBase.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc
{
    Command::Command(
        std::wstring_view name,
        std::vector<std::wstring_view> aliases,
        std::wstring parent,
        Command::Visibility visibility,
        CommandOutputFlags outputFlags) :
        m_name(name), m_aliases(std::move(aliases)), m_visibility(visibility), m_outputFlags(outputFlags)
    {
        if (!parent.empty())
        {
            m_fullName.reserve(parent.length() + 1 + name.length());
            m_fullName = parent;
            m_fullName += ParentSplitChar;
            m_fullName += name;
        }
        else
        {
            m_fullName = name;
        }
    }

    void Command::OutputIntroHeader() const
    {
        // TODO: Product name, version, copyright info in resources.
        std::wostringstream infoOut;
        infoOut << L"Windows Subsystem for Linux Container CLI (Preview) v1.0.0" << std::endl;
        infoOut << L"Copyright (c) Microsoft Corporation. All rights reserved." << std::endl;
        PrintMessage(infoOut.str(), stdout);
    }

    void Command::OutputHelp(const CommandException* exception) const
    {
        // Header
        OutputIntroHeader();

        // Error if given
        if (exception)
        {
            PrintMessage(exception->Message(), stderr);
        }

        // Description
        std::wostringstream infoOut;
        infoOut <<
            LongDescription() << std::endl <<
            std::endl;

        // Example usage for this command
        // First create the command chain for output
        std::wstring commandChain = FullName();
        size_t firstSplit = commandChain.find_first_of(ParentSplitChar);
        if (firstSplit == std::wstring::npos)
        {
            commandChain.clear();
        }
        else
        {
            commandChain = commandChain.substr(firstSplit + 1);
            for (wchar_t& c : commandChain)
            {
                if (c == ParentSplitChar)
                {
                    c = L' ';
                }
            }
        }

        // Output the command preamble and command chain
        infoOut << Localization::WSLCCLI_Usage(L"wslc2", std::wstring_view{commandChain});

        auto commandAliases = Aliases();
        auto commands = GetVisibleCommands();
        auto arguments = GetVisibleArguments();

        bool hasArguments = false;
        bool hasOptions = false;

        // Output the command token, made optional if arguments are present.
        if (!commands.empty())
        {
            infoOut << ' ';

            if (!arguments.empty())
            {
                infoOut << L'[';
            }

            infoOut << L'<' << Localization::WSLCCLI_Command() << L'>';

            if (!arguments.empty())
            {
                infoOut << L']';
            }
        }

        // Arguments are required by a test to have all positionals first.
        // TODO: Need to adjust this for wslc format.
        // for WSLC format of command <options> <positional> <args | positional2..>
        for (const auto& arg : arguments)
        {
            if (arg.Type() == ArgumentType::Positional)
            {
                hasArguments = true;

                infoOut << L' ';

                if (!arg.Required())
                {
                    infoOut << L'[';
                }

                infoOut << L'[';

                if (arg.Alias() == ArgumentCommon::NoAlias)
                {
                    infoOut << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << arg.Name();
                }
                else
                {
                    infoOut << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << arg.Alias();
                }

                infoOut << L"] <" << arg.Name() << L'>';

                if (arg.Limit() > 1)
                {
                    infoOut << L"...";
                }

                if (!arg.Required())
                {
                    infoOut << L']';
                }
            }
            else
            {
                hasOptions = true;
                infoOut << L" [<" << Localization::WSLCCLI_Options()<< L">]";
                break;
            }
        }

        infoOut <<
            std::endl <<
            std::endl;

        if (!commandAliases.empty())
        {
            infoOut << Localization::WSLCCLI_AvailableCommandAliases() << std::endl;
            
            for (const auto& commandAlias : commandAliases)
            {
                infoOut << L"  " << commandAlias << std::endl;
            }
            infoOut << std::endl;
        }

        if (!commands.empty())
        {
            if (Name() == FullName())
            {
                infoOut << Localization::WSLCCLI_AvailableCommands() << std::endl;
            }
            else
            {
                infoOut << Localization::WSLCCLI_AvailableSubcommands() << std::endl;
            }

            size_t maxCommandNameLength = 0;
            for (const auto& command : commands)
            {
                maxCommandNameLength = std::max(maxCommandNameLength, command->Name().length());
            }

            for (const auto& command : commands)
            {
                size_t fillChars = (maxCommandNameLength - command->Name().length()) + 2;
                infoOut << L"  " << command->Name() << std::wstring(fillChars, L' ') << command->ShortDescription() << std::endl;
            }

            infoOut << std::endl << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARGUMENT << L']' << std::endl;
        }

        if (!arguments.empty())
        {
            if (!commands.empty())
            {
                infoOut << std::endl;
            }

            std::vector<std::wstring> argNames;
            size_t maxArgNameLength = 0;
            for (const auto& arg : arguments)
            {
                argNames.emplace_back(arg.GetUsageString());
                maxArgNameLength = std::max(maxArgNameLength, argNames.back().length());
            }

            if (hasArguments)
            {
                infoOut << Localization::WSLCCLI_AvailableArguments() << std::endl;

                size_t i = 0;
                for (const auto& arg : arguments)
                {
                    const std::wstring& argName = argNames[i++];
                    if (arg.Type() == ArgumentType::Positional)
                    {
                        size_t fillChars = (maxArgNameLength - argName.length()) + 2;
                        infoOut << L"  " << argName << std::wstring(fillChars, ' ') << arg.Description() << std::endl;
                    }
                }
            }

            if (hasOptions)
            {
                if (hasArguments)
                {
                    infoOut << std::endl;
                }

                infoOut << Localization::WSLCCLI_AvailableOptions() << std::endl;

                size_t i = 0;
                for (const auto& arg : arguments)
                {
                    const std::wstring& argName = argNames[i++];
                    if (arg.Type() != ArgumentType::Positional)
                    {
                        size_t fillChars = (maxArgNameLength - argName.length()) + 2;
                        infoOut << L"  " << argName << std::wstring(fillChars, ' ') << arg.Description() << std::endl;
                    }
                }
            }
        }

        PrintMessage(infoOut.str(), stdout);
    }

    std::unique_ptr<Command> Command::FindSubCommand(Invocation& inv) const
    {
        auto itr = inv.begin();
        if (itr == inv.end() || (*itr)[0] == WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR)
        {
            // No more command arguments to check, so no command to find
            return {};
        }

        auto commands = GetCommands();
        if (commands.empty())
        {
            // No more subcommands
            return {};
        }

        for (auto& command : commands)
        {
            if (string::IsEqual(*itr, command->Name()))
            {
                inv.consume(itr);
                return std::move(command);
            }
        }

        // The command has opted-in to be executed when it has subcommands and the next token is a positional parameter value
        if (m_selectCurrentCommandIfUnrecognizedSubcommandFound)
        {
            return {};
        }

        // TODO: If we get to a large number of commands, do a fuzzy search much like git
        throw CommandException(Localization::WSLCCLI_UnrecognizedCommandError(std::wstring_view{*itr}));
    }

    // The argument parsing state machine.
    // It is broken out to enable completion to process arguments, ignore errors,
    // and determine the likely state of the word to be completed.
    struct ParseArgumentsStateMachine
    {
        ParseArgumentsStateMachine(Invocation& inv, Args& execArgs, std::vector<Argument> arguments);

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
            State(Args::Type type, std::wstring_view arg) : m_type(type), m_arg(arg) {}
            State(CommandException ce) : m_exception(std::move(ce)) {}

            // If set, indicates that the next argument is a value for this type.
            const std::optional<Args::Type>& Type() const { return m_type; }

            // The actual argument string associated with Type.
            const std::wstring& Arg() const { return m_arg; }

            // If set, indicates that the last argument produced an error.
            const std::optional<CommandException>& Exception() const { return m_exception; }

        private:
            std::optional<Args::Type> m_type;
            std::wstring m_arg;
            std::optional<CommandException> m_exception;
        };

        const State& GetState() const { return m_state; }

        bool OnlyPositionalRemain() const { return m_onlyPositionalArgumentsRemain; }

        // Gets the next positional argument, or nullptr if there is not one.
        const Argument* NextPositional();

        const std::vector<Argument>& Arguments() const { return m_arguments; }

    private:
        State StepInternal();

        void ProcessAdjoinedValue(Args::Type type, std::wstring_view value);

        Invocation& m_invocation;
        Args& m_executionArgs;
        std::vector<Argument> m_arguments;

        Invocation::iterator m_invocationItr;
        std::vector<Argument>::iterator m_positionalSearchItr;
        bool m_onlyPositionalArgumentsRemain = false;

        State m_state;
    };

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
            throw CommandException(L"WSLCCLI_MissingArgumentError" /* m_state.Arg() */);
        }
    }

    const Argument* ParseArgumentsStateMachine::NextPositional()
    {
        // Find the next appropriate positional arg if the current itr isn't one or has hit its limit.
        while (m_positionalSearchItr != m_arguments.end() &&
            (m_positionalSearchItr->Type() != ArgumentType::Positional || m_executionArgs.GetCount(m_positionalSearchItr->ExecArgType()) == m_positionalSearchItr->Limit()))
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
    //  4. If the argument is only a double --, all further arguments are only considered as positional.
    ParseArgumentsStateMachine::State ParseArgumentsStateMachine::StepInternal()
    {
        auto currArg = std::wstring_view{ *m_invocationItr };
        ++m_invocationItr;

        // If the previous step indicated a value was needed, set it and forget it.
        if (m_state.Type())
        {
            m_executionArgs.AddArg(m_state.Type().value(), currArg);
            return {};
        }

        // This is a positional argument
        if (m_onlyPositionalArgumentsRemain || currArg.empty() || currArg[0] != WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR)
        {
            const Argument* nextPositional = NextPositional();
            if (!nextPositional)
            {
                return CommandException(L"WSLCCLI_ExtraPositionalError" /* currArg */);
            }

            m_executionArgs.AddArg(nextPositional->ExecArgType(), currArg);
        }
        // The currentArg must not be empty, and starts with a -
        else if (currArg.length() == 1)
        {
            return CommandException(L"WSLCCLI_InvalidArgumentSpecifierError" /* currArg */);
        }
        // Now it must be at least 2 chars
        else if (currArg[1] != WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR)
        {
            // Parse the single character alias argument
            auto currChar = currArg[1];

            auto itr = std::find_if(m_arguments.begin(), m_arguments.end(), [&](const Argument& arg) { return (currChar == arg.Alias()); });
            if (itr == m_arguments.end())
            {
                return CommandException(L"WSLCCLI_InvalidAliasError" /* currArg */);
            }

            if (itr->Type() == ArgumentType::Flag)
            {
                m_executionArgs.AddArg(itr->ExecArgType());

                for (size_t i = 2; i < currArg.length(); ++i)
                {
                    currChar = currArg[i];

                    auto itr2 = std::find_if(m_arguments.begin(), m_arguments.end(), [&](const Argument& arg) { return (currChar == arg.Alias()); });
                    if (itr2 == m_arguments.end())
                    {
                        return CommandException(L"WSLCCLI_AdjoinedNotFoundError" /* currArg */);
                    }
                    else if (itr2->Type() != ArgumentType::Flag)
                    {
                        return CommandException(L"WSLCCLI_AdjoinedNotFlagError" /* currArg */);
                    }
                    else
                    {
                        m_executionArgs.AddArg(itr2->ExecArgType());
                    }
                }
            }
            else if (currArg.length() > 2)
            {
                if (currArg[2] == WSLC_CLI_ARGUMENT_SPLIT_CHAR)
                {
                    ProcessAdjoinedValue(itr->ExecArgType(), currArg.substr(3));
                }
                else
                {
                    return CommandException(L"WSLCCLI_SingleCharAfterDashError" /* currArg */);
                }
            }
            else
            {
                return { itr->ExecArgType(), currArg };
            }
        }
        // The currentArg is at least 2 chars, both of which are --
        else if (currArg.length() == 2)
        {
            m_onlyPositionalArgumentsRemain = true;
        }
        // The currentArg is more than 2 chars, both of which are --
        else
        {
            // This is an arg name, find it and process its value if needed.
            // Skip the double arg identifier chars.
            size_t argStart = currArg.find_first_not_of(WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR);
            std::wstring_view argName = currArg.substr(argStart);
            bool argFound = false;

            bool hasValue = false;
            std::wstring_view argValue;
            size_t splitChar = argName.find_first_of(WSLC_CLI_ARGUMENT_SPLIT_CHAR);
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
                    if (arg.Type() == ArgumentType::Flag)
                    {
                        if (hasValue)
                        {
                            return CommandException(L"WSLCCLI_FlagContainAdjoinedError" /* currArg */);
                        }

                        m_executionArgs.AddArg(arg.ExecArgType());
                    }
                    else if (hasValue)
                    {
                        ProcessAdjoinedValue(arg.ExecArgType(), argValue);
                    }
                    else
                    {
                        return { arg.ExecArgType(), currArg };
                    }
                    argFound = true;
                    break;
                }
            }

            if (!argFound)
            {
                return CommandException(Localization::WSLCCLI_InvalidNameError(currArg));
            }
        }

        // If we get here, the next argument can be anything again.
        return {};
    }

    void ParseArgumentsStateMachine::ProcessAdjoinedValue(Args::Type type, std::wstring_view value)
    {
        // If the adjoined value is wrapped in quotes, strip them off.
        if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
        {
            value = value.substr(1, value.length() - 2);
        }

        m_executionArgs.AddArg(type, std::wstring{ value });
    }

    void Command::ParseArguments(Invocation& inv, Args& execArgs) const
    {
        auto definedArgs = GetArguments();
        Argument::GetCommon(definedArgs);

        ParseArgumentsStateMachine stateMachine{ inv, execArgs, std::move(definedArgs) };

        while (stateMachine.Step())
        {
            stateMachine.ThrowIfError();
        }
    }

    void Command::ValidateArguments(Args& execArgs) const
    {
        // If help is asked for, don't bother validating anything else
        if (execArgs.Contains(Args::Type::Help))
        {
            return;
        }

        // Common arguments need to be validated with command arguments, as there may be common arguments blocked by Experimental Feature or Group Policy
        auto allArgs = GetArguments();
        Argument::GetCommon(allArgs);

        for (const auto& arg : allArgs)
        {
            if (arg.Required() && !execArgs.Contains(arg.ExecArgType()))
            {
                throw CommandException(Localization::WSLCCLI_RequiredArgumentError(arg.Name()));
            }

            if (arg.Limit() < execArgs.GetCount(arg.ExecArgType()))
            {
                throw CommandException(Localization::WSLCCLI_TooManyArgumentsError(arg.Name()));
            }
        }

        Argument::ValidateExclusiveArguments(execArgs);

        ValidateArgumentsInternal(execArgs);
    }

    void Command::Execute(CLIExecutionContext& context) const
    {
        if (context.Args.Contains(Args::Type::Help))
        {
            OutputHelp();
        }
        else
        {
            ExecuteInternal(context);
        }
    }

    void Command::SelectCurrentCommandIfUnrecognizedSubcommandFound(bool value)
    {
        m_selectCurrentCommandIfUnrecognizedSubcommandFound = value;
    }

    void Command::ValidateArgumentsInternal(Args&) const
    {
        // Do nothing by default.
        // Commands may not need any extra validation.
    }

    void Command::ExecuteInternal(CLIExecutionContext& context) const
    {
        // This is a developer error if we get here, should never be user-facing.
        PrintMessage(L"ExecuteInternal for command '" + FullName() + L"' not implemented.\n", stdout);
        THROW_HR(E_NOTIMPL);
    }

    Command::Visibility Command::GetVisibility() const
    {
        return m_visibility;
    }

    std::vector<std::unique_ptr<Command>> Command::GetVisibleCommands() const
    {
        auto commands = GetCommands();

        commands.erase(
            std::remove_if(
                commands.begin(), commands.end(),
                [](const std::unique_ptr<Command>& command) { return command->GetVisibility() == Command::Visibility::Hidden; }),
            commands.end());

        return commands;
    }

    std::vector<Argument> Command::GetVisibleArguments() const
    {
        auto arguments = GetArguments();
        Argument::GetCommon(arguments);

        arguments.erase(
            std::remove_if(
                arguments.begin(), arguments.end(),
                [](const Argument& arg) { return arg.GetVisibility() == Argument::Visibility::Hidden; }),
            arguments.end());

        return arguments;
    }

    // This is the main execution wrapper for a command. It will catch any exceptions and set the return code
    // based on the exception and/or results of the command execution.
    void ExecuteWithoutLoggingSuccess(CLIExecutionContext& context, Command* command)
    {
        try
        {
            command->Execute(context);
        }
        catch (...)
        {
            context.SetTerminationHR(task::HandleException(context, std::current_exception()));
        }
    }

    int Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command)
    {
        ExecuteWithoutLoggingSuccess(context, command.get());

        if (SUCCEEDED(context.GetTerminationHR()))
        {
            ////Logging::Telemetry().LogCommandSuccess(command->FullName());
        }

        return context.GetTerminationHR();
    }
}
