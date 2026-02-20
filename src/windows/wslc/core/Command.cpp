/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Command.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "Argument.h"
#include "Command.h"
#include "Invocation.h"
#include "ArgumentParser.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
constexpr std::wstring_view s_ExecutableName = L"wslc";

Command::Command(std::wstring_view name, std::vector<std::wstring_view>&& aliases, const std::wstring& parent) :
    m_name(name), m_aliases(std::move(aliases))
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

    auto commandAliases = Aliases();
    auto commands = GetCommands();
    auto arguments = GetAllArguments();

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

    if (!commandAliases.empty())
    {
        infoOut << Localization::WSLCCLI_AvailableCommandAliases() << L' ';
        infoOut << string::Join(commandAliases, L' ');
        infoOut << std::endl << std::endl;
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
        return {};
    }

    for (auto& command : commands)
    {
        if (string::IsEqual(*itr, command->Name()))
        {
            inv.consume(itr);
            return std::move(command);
        }

        for (const auto& alias : command->Aliases())
        {
            if (string::IsEqual(*itr, alias))
            {
                inv.consume(itr);
                return std::move(command);
            }
        }
    }

    throw CommandException(Localization::WSLCCLI_UnrecognizedCommandError(std::wstring_view{*itr}));
}

// Convert the invocation vector into a map of argument types and their associated values.
// Argument map is based on the arguments that the command defines and are stored as
// an enum -> variant multimap. This is parsing and value storage only, not validation of
// the argument data.
void Command::ParseArguments(Invocation& inv, ArgMap& execArgs) const
{
    auto definedArgs = GetAllArguments();

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
    // If help is asked for, don't bother validating anything else.
    if (execArgs.Contains(ArgType::Help))
    {
        return;
    }

    auto allArgs = GetAllArguments();
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
    }

    ValidateArgumentsInternal(execArgs);
}

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

// External execution entry point called by the core execution flow.
void Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command)
{
    command->Execute(context);
}

void Command::ValidateArgumentsInternal(const ArgMap&) const
{
    // Commands may not need any extra validation; they'll override if they do.
}
} // namespace wsl::windows::wslc
