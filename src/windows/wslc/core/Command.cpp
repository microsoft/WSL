/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Command.cpp

Abstract:

    Implementation of command execution logic.

--*/
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

namespace wsl::windows::wslc {
constexpr std::wstring_view s_ExecutableName = L"wslc";

Command::Command(std::wstring_view name, std::wstring parent, Command::Visibility visibility) :
    m_name(name), m_visibility(visibility)
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

// This is the header applied before every help output, for product and copyright information.
// It is separate in case we need to show it in other contexts, such as error messages, or
// during specific command executions.
void Command::OutputIntroHeader() const
{
    // Placeholder header.
    // TODO: Get better product version information dynamically instead of hardcoding it here.
    // TODO: Strings should be in resources.
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
    infoOut << LongDescription() << std::endl << std::endl;

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
    infoOut << Localization::WSLCCLI_Usage(s_ExecutableName, std::wstring_view{commandChain});

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
        case Kind::Flag:
            standardArgs.emplace_back(arg);
            break;
        case Kind::Value:
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
        infoOut << L" [<" << Localization::WSLCCLI_Options() << L">]";
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

    infoOut << std::endl << std::endl;

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

        infoOut << std::endl << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L']' << std::endl;
    }

    if (!arguments.empty())
    {
        if (!commands.empty())
        {
            infoOut << std::endl;
        }

        size_t maxArgNameLength = 0;
        for (const auto& arg : arguments)
        {
            auto argLength = arg.GetUsageString().length();
            maxArgNameLength = std::max(maxArgNameLength, argLength);
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

    // TODO: If we get to a large number of commands, do a fuzzy search much like git
    throw CommandException(Localization::WSLCCLI_UnrecognizedCommandError(std::wstring_view{*itr}));
}

// Convert the invocation vector into a map of argument types and their associated values.
// Argument map is based on the arguments that the command defines and are stored as
// an enum -> variant multimap. This is parsing and value storage only, not validation of
// the argument data.
void Command::ParseArguments(Invocation& inv, ArgMap& execArgs) const
{
    auto definedArgs = GetArguments();
    Argument::GetCommon(definedArgs);

    ParseArgumentsStateMachine stateMachine{inv, execArgs, std::move(definedArgs)};

    while (stateMachine.Step())
    {
        stateMachine.ThrowIfError();
    }
}

// Validates the ArgMap produced by ParseArguments. ArgMap is assumed to have
// been populated and parsed successfully from the invocation and now we are validating
// that the arguments provided meet the requirements of the command. This includes checking
// that all required arguments are present and no arguments exceed their count limits.
// Any defined validation for specific ArgTypes are also run.
void Command::ValidateArguments(ArgMap& execArgs) const
{
    // If help is asked for, don't bother validating anything else
    if (execArgs.Contains(ArgType::Help))
    {
        return;
    }

    // Common arguments need to be validated with command arguments
    auto allArgs = GetArguments();
    Argument::GetCommon(allArgs);

    for (const auto& arg : allArgs)
    {
        if (arg.Required() && !execArgs.Contains(arg.Type()))
        {
            throw CommandException(Localization::WSLCCLI_RequiredArgumentError(arg.Name()));
        }

        if (arg.Limit() < execArgs.Count(arg.Type()))
        {
            throw CommandException(Localization::WSLCCLI_TooManyArgumentsError(arg.Name()));
        }

        // Call type-specific validation for each argument.
        if (execArgs.Contains(arg.Type()))
        {
            arg.Validate(execArgs);
        }
    }

    ValidateArgumentsInternal(execArgs);
}

// This enables the command to do any optional validation that is specific to the command and
// not otherwise covered by type-specific or common argument validation.
void Command::ValidateArgumentsInternal(ArgMap&) const
{
    // Do nothing by default.
    // Commands may not need any extra validation.
}

// Assumed to be called after all arguments have been parsed and validated.
void Command::Execute(CLIExecutionContext& context) const
{
    // If Help was part of the validated argument set, we will output help instead of executing.
    if (context.Args.Contains(ArgType::Help))
    {
        OutputHelp();
    }
    else
    {
        // Execute internal has the actual command execution path.
        ExecuteInternal(context);
    }
}

// Commands must override this and provide an implementation.
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

// Filters subcommands to only the visible set. Used by OutputHelp to not include hidden subcommands.
std::vector<std::unique_ptr<Command>> Command::GetVisibleCommands() const
{
    auto commands = GetCommands();

    commands.erase(
        std::remove_if(
            commands.begin(),
            commands.end(),
            [](const std::unique_ptr<Command>& command) { return command->GetVisibility() == Command::Visibility::Hidden; }),
        commands.end());

    return commands;
}

// Filters arguments to only the visible set. Used by OutputHelp to not include hidden arguments.
std::vector<Argument> Command::GetVisibleArguments() const
{
    auto arguments = GetArguments();
    Argument::GetCommon(arguments);

    arguments.erase(
        std::remove_if(
            arguments.begin(),
            arguments.end(),
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

// External execution entry point called by the core execution flow. Errors are expected to be caught and
// and handled by ExecuteWithoutLoggingSuccess, with appropriate logging of the errors and successful
// execution of the commands.
int Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command)
{
    ExecuteWithoutLoggingSuccess(context, command.get());

    if (SUCCEEDED(context.GetTerminationHR()))
    {
        ////Logging::Telemetry().LogCommandSuccess(command->FullName());
    }

    return context.GetTerminationHR();
}
} // namespace wsl::windows::wslc
