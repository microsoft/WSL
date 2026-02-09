// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Argument.h"
#include "Command.h"
#include "Invocation.h"
#include "TaskBase.h"
#include "ArgumentParser.h"

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

        // Usage follows the Microsoft convention:
        // https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/command-line-syntax-key

        // Output the command preamble and command chain
        infoOut << Localization::WSLCCLI_Usage(L"wslc2", std::wstring_view{commandChain});

        auto commandAliases = Aliases();
        auto commands = GetVisibleCommands();
        auto arguments = GetVisibleArguments();

        // Separate arguments by Kind
        std::vector<Argument> standardArgs;
        std::vector<Argument> positionalArgs;
        std::vector<Argument> forwardArgs;
        bool requiredPositionalArgsExist = false;
        for (const auto& arg : arguments)
        {
            switch (arg.Kind())
            {
                case Kind::Standard:
                    standardArgs.emplace_back(arg);
                    break;
                case Kind::Positional:
                    positionalArgs.emplace_back(arg);
                    if (arg.Required())
                    {
                        requiredPositionalArgsExist = true;
                    }
                    break;
                case Kind::Forward:
                    forwardArgs.emplace_back(arg);
                    break;
            }
        }

        bool hasArguments = !positionalArgs.empty();
        bool hasOptions = !standardArgs.empty();
        bool hasForwardArgs = !forwardArgs.empty();

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

        // For WSLC format of command [<options>] <positional> <args | positional2..>


        // Add options to the usage if there are options present.
        if (hasOptions)
        {
            infoOut << L" [<" << Localization::WSLCCLI_Options()<< L">]";
        }

        // Add arguments to the usage if there are arguments present. Positional come after
        // options and may be optional or required.
        for (const auto& arg : positionalArgs)
        {   
            infoOut << L' ';

            if (!arg.Required())
            {
                infoOut << L'[';
            }

            infoOut << L'<' << arg.Name() << L'>';

            if (arg.Limit() > 1)
            {
                infoOut << L"...";
            }

            if (!arg.Required())
            {
                infoOut << L']';
            }
        }

        if (hasForwardArgs)
        {
            // Assume only one forward arg is present, as multiple forwards would be
            // ambiguous in usage. Revisit if this becomes a scenario.
            infoOut << L" [<" << forwardArgs.front().Name() << L">...]";
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

            infoOut << std::endl << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_HELP_CHAR << L']' << std::endl;
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

                for (const auto& arg : positionalArgs)
                {
                    size_t fillChars = (maxArgNameLength - arg.Name().length()) + 2;
                    infoOut << L"  " << arg.Name() << std::wstring(fillChars, ' ') << arg.Description() << std::endl;
                }
            }

            if (hasForwardArgs)
            {
                ////infoOut << Localization::WSLCCLI_AvailableForwardArguments() << std::endl;
                for (const auto& arg : forwardArgs)
                {
                    size_t fillChars = (maxArgNameLength - arg.Name().length()) + 2;
                    infoOut << L"  " << arg.Name() << std::wstring(fillChars, ' ') << arg.Description() << std::endl;
                }
            }

            if (hasOptions)
            {
                if (hasArguments || hasForwardArgs)
                {
                    infoOut << std::endl;
                }

                infoOut << Localization::WSLCCLI_AvailableOptions() << std::endl;
                for (const auto& arg : standardArgs)
                {
                    auto usage = arg.GetUsageString();
                    size_t fillChars = (maxArgNameLength - usage.length()) + 2;
                    infoOut << L"  " << usage << std::wstring(fillChars, ' ') << arg.Description() << std::endl;
                }
            }
        }

        PrintMessage(infoOut.str(), stdout);
    }

    std::unique_ptr<Command> Command::FindSubCommand(Invocation& inv) const
    {
        auto itr = inv.begin();
        if (itr == inv.end() || (*itr)[0] == WSLC_CLI_ARG_ID_CHAR)
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
        // Change from Args::Type::Help to ArgType::Help
        if (execArgs.Contains(ArgType::Help))
        {
            return;
        }

        // Common arguments need to be validated with command arguments, as there may be common arguments blocked by Experimental Feature or Group Policy
        auto allArgs = GetArguments();
        Argument::GetCommon(allArgs);

        for (const auto& arg : allArgs)
        {
            if (arg.Required() && !execArgs.Contains(arg.Type()))
            {
                throw CommandException(Localization::WSLCCLI_RequiredArgumentError(arg.Name()));
            }

            /* if (arg.Limit() < execArgs.Contains(arg.Type()))
            {
                throw CommandException(Localization::WSLCCLI_TooManyArgumentsError(arg.Name()));
            }*/
        }

        Argument::ValidateExclusiveArguments(execArgs);

        ValidateArgumentsInternal(execArgs);
    }

    void Command::Execute(CLIExecutionContext& context) const
    {
        if (context.Args.Contains(ArgType::Help))
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
                [](const Argument& arg) { return arg.GetVisibility() == argument::Visibility::Hidden; }),
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
