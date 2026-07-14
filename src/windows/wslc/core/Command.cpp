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
#include "RootCommand.h"
#include "TableOutput.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::common::vt;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {

std::wstring s_ExecutableName = L"wslc";

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

void Command::OutputHelp(Reporter& reporter, const CommandException* exception) const
{
    constexpr size_t c_helpRowIndent = 2;
    constexpr size_t c_helpColumnPadding = 2;
    const auto helpLevel = exception ? Reporter::Level::Info : Reporter::Level::Output;

    // Emphasis sequences for help output.
    static const auto& HelpHeadingEmphasis = Format::Bright;
    static const auto& HelpCommandEmphasis = Format::Bright;
    static const auto& HelpArgumentEmphasis = Format::Bright;
    static const auto& HelpMetaEmphasis = Format::Dim;
    static const auto& HelpPlaceholderEmphasis = Format::Fg::BrightCyan;

    // Copyright header (dimmed)
    reporter.Write(helpLevel, L"{}{}{}\n\n", HelpMetaEmphasis, Localization::WSLCCLI_CopyrightHeader(), Format::Default);

    // Error if given
    if (exception)
    {
        reporter.Error(L"{}\n\n", exception->Message());
    }

    // Description
    reporter.Write(helpLevel, L"{}\n\n", LongDescription());

    // Build command chain from full name (replace ParentSplitChar with spaces, strip root).
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

    auto commandAliases = Aliases();
    auto commands = GetCommands();
    auto arguments = GetAllArguments();

    std::vector<Argument> standardArgs;
    std::vector<Argument> positionalArgs;
    std::vector<Argument> forwardArgs;
    for (const auto& arg : arguments)
    {
        switch (arg.Kind())
        {
        case Kind::Flag:
        case Kind::Value:
            standardArgs.emplace_back(arg);
            break;
        case Kind::Positional:
            positionalArgs.emplace_back(arg);
            break;
        case Kind::Forward:
            forwardArgs.emplace_back(arg);
            break;
        }
    }

    const bool hasArguments = !positionalArgs.empty();
    const bool hasOptions = !standardArgs.empty();
    const bool hasForwardArgs = !forwardArgs.empty();

    // Global options from the root command, shown on every command's help.
    auto globalArgs = RootCommand().GetGlobalArguments();

    // Build usage line with Write calls for each segment.
    {
        std::wstring usageText = Localization::WSLCCLI_Usage(s_ExecutableName, std::wstring_view{commandChain});

        while (!usageText.empty() && usageText.back() == L' ')
        {
            usageText.pop_back();
        }

        reporter.Write(helpLevel, L"{}{}{}", HelpHeadingEmphasis, usageText, Format::Default);

        if (!commands.empty())
        {
            if (!arguments.empty())
            {
                reporter.Write(helpLevel, L" {}[{}", HelpMetaEmphasis, Format::Default);
            }
            else
            {
                reporter.Write(helpLevel, L" ");
            }

            reporter.Write(
                helpLevel,
                L"{}<{}{}{}{}{}>{}",
                HelpMetaEmphasis,
                Format::Default,
                HelpPlaceholderEmphasis,
                Localization::WSLCCLI_Command(),
                Format::Default,
                HelpMetaEmphasis,
                Format::Default);
            if (!arguments.empty())
            {
                reporter.Write(helpLevel, L"{}]{}", HelpMetaEmphasis, Format::Default);
            }
        }

        if (hasOptions)
        {
            reporter.Write(
                helpLevel,
                L" {}[<{}{}{}{}{}>]{}",
                HelpMetaEmphasis,
                Format::Default,
                HelpPlaceholderEmphasis,
                Localization::WSLCCLI_Options(),
                Format::Default,
                HelpMetaEmphasis,
                Format::Default);
        }

        for (const auto& arg : positionalArgs)
        {
            reporter.Write(helpLevel, L" ");
            if (!arg.Required())
            {
                reporter.Write(helpLevel, L"{}[{}", HelpMetaEmphasis, Format::Default);
            }

            reporter.Write(
                helpLevel, L"{}<{}{}{}{}{}>{}", HelpMetaEmphasis, Format::Default, HelpPlaceholderEmphasis, arg.Name(), Format::Default, HelpMetaEmphasis, Format::Default);
            if (arg.Limit() > 1)
            {
                reporter.Write(helpLevel, L"{}...{}", HelpMetaEmphasis, Format::Default);
            }

            if (!arg.Required())
            {
                reporter.Write(helpLevel, L"{}]{}", HelpMetaEmphasis, Format::Default);
            }
        }

        if (hasForwardArgs)
        {
            reporter.Write(
                helpLevel,
                L" {}[<{}{}{}{}{}>...]{}",
                HelpMetaEmphasis,
                Format::Default,
                HelpPlaceholderEmphasis,
                forwardArgs.front().Name(),
                Format::Default,
                HelpMetaEmphasis,
                Format::Default);
        }

        reporter.Write(helpLevel, L"\n\n");
    }

    if (!commandAliases.empty())
    {
        reporter.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingAliases(), Format::Default);

        std::wstring aliasLine;
        for (size_t i = 0; i < commandAliases.size(); ++i)
        {
            if (i != 0)
            {
                aliasLine += L", ";
            }
            aliasLine += commandAliases[i];
        }

        reporter.Write(helpLevel, L"{}{}\n\n", std::wstring(c_helpRowIndent, L' '), aliasLine);
    }

    // Col0: name/command
    // Col1: description (word-wraps at computed column width)
    const auto MakeHelpTable = [&reporter, helpLevel]() -> TableOutput<2> {
        TableOutput<2> table{reporter, {L"", L""}, 50, c_helpColumnPadding, helpLevel};
        table.SetShowHeader(false);
        table.SetRowIndent(c_helpRowIndent);
        table.SetColumnConfig(
            1,
            ColumnWidthConfig{
                .MinWidth = ColumnWidthConfig::NoLimit,
                .MaxWidth = ColumnWidthConfig::NoLimit,
                .Overflow = ColumnOverflow::Wrap,
            });
        return table;
    };

    if (!commands.empty())
    {
        reporter.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingCommands(), Format::Default);

        auto table = MakeHelpTable();
        for (const auto& command : commands)
        {
            table.WriteRow({
                FormattedCell(command->Name(), HelpCommandEmphasis),
                FormattedCell(command->ShortDescription()),
            });
        }
        table.Complete();

        reporter.Write(helpLevel, L"\n{} [{}]\n", Localization::WSLCCLI_HelpForDetails(), WSLC_CLI_HELP_ARG_STRING);
    }

    if (!arguments.empty())
    {
        if (!commands.empty())
        {
            reporter.Write(helpLevel, L"\n");
        }

        // Arguments table: positional and forward args, name (emphasized) | description
        if (hasArguments || hasForwardArgs)
        {
            reporter.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingArguments(), Format::Default);

            auto table = MakeHelpTable();

            for (const auto& arg : positionalArgs)
            {
                table.WriteRow({
                    FormattedCell(arg.Name(), HelpArgumentEmphasis),
                    FormattedCell(arg.Description()),
                });
            }

            for (const auto& arg : forwardArgs)
            {
                table.WriteRow({
                    FormattedCell(arg.Name(), HelpArgumentEmphasis),
                    FormattedCell(arg.Description()),
                });
            }

            table.Complete();
        }
    }

    // Col0: short alias (e.g. "-f")
    // Col1: long name  (e.g. "--force")
    // Col2: description (word-wraps at computed column width)
    const auto MakeOptionsTable = [&reporter, helpLevel]() -> TableOutput<3> {
        TableOutput<3> table{reporter, {L"", L"", L""}, {}, 50, c_helpColumnPadding, helpLevel};
        table.SetShowHeader(false);
        table.SetRowIndent(c_helpRowIndent);
        table.SetColumnConfig(
            2,
            ColumnWidthConfig{
                .MinWidth = ColumnWidthConfig::NoLimit,
                .MaxWidth = ColumnWidthConfig::NoLimit,
                .Overflow = ColumnOverflow::Wrap,
            });
        return table;
    };

    // Options table: alias (emphasized) | long name (emphasized) | description
    // Global options are appended to the same table so column widths are shared.
    if (hasOptions || !globalArgs.empty())
    {
        if (hasArguments || hasForwardArgs)
        {
            reporter.Write(helpLevel, L"\n");
        }
        else if (!commands.empty() && arguments.empty())
        {
            reporter.Write(helpLevel, L"\n");
        }

        auto table = MakeOptionsTable();

        const auto AddOptionRows = [&table](const std::vector<Argument>& args) {
            for (const auto& arg : args)
            {
                FormattedCell aliasCell{L""};
                if (!arg.Alias().empty())
                {
                    aliasCell = FormattedCell(std::wstring{WSLC_CLI_ARG_ID_CHAR} + arg.Alias(), HelpArgumentEmphasis);
                }

                table.WriteRow({
                    std::move(aliasCell),
                    FormattedCell(std::wstring{WSLC_CLI_ARG_ID_CHAR} + std::wstring{WSLC_CLI_ARG_ID_CHAR} + arg.Name(), HelpArgumentEmphasis),
                    FormattedCell(arg.Description()),
                });
            }
        };

        if (hasOptions)
        {
            table.WriteLine(FormattedCell(Localization::WSLCCLI_HeadingOptions(), HelpHeadingEmphasis));
            AddOptionRows(standardArgs);
        }

        if (!globalArgs.empty())
        {
            if (hasOptions)
            {
                table.WriteLine();
            }
            table.WriteLine(FormattedCell(Localization::WSLCCLI_HeadingGlobalOptions(), HelpHeadingEmphasis));
            AddOptionRows(globalArgs);
        }

        table.Complete();
    }
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
void Command::ParseArguments(
    Invocation& inv, ArgMap& target, std::vector<Argument> definedArgs, bool optionsOnly, bool stopOnUnknown, const std::vector<Argument>& overridableDefaults) const
{
    if (definedArgs.empty())
    {
        return;
    }

    ParseArgumentsStateMachine stateMachine{inv, target, std::move(definedArgs), optionsOnly, stopOnUnknown, overridableDefaults};

    while (stateMachine.Step())
    {
        stateMachine.ThrowIfError();
    }
    stateMachine.ThrowIfError();

    // Both modes leave the iterator at the first unconsumed token; sync inv.
    if (optionsOnly || stopOnUnknown)
    {
        inv.consumeUntil(stateMachine.Position());
    }
}

// Validates the ArgMap produced by ParseArguments. ArgMap is assumed to have
// been populated and parsed successfully from the invocation and now we are validating
// that the arguments provided meet the requirements of the command. This includes checking
// that all required arguments are present and no arguments exceed their count limits.
// Any defined validation for specific ArgTypes are also run.
void Command::ValidateArguments(const ArgMap& source, const std::vector<Argument>& definedArgs, bool runInternalHook) const
{
    if (source.Contains(ArgType::Help))
    {
        return;
    }

    for (const auto& arg : definedArgs)
    {
        if (arg.Required() && !source.Contains(arg.Type()))
        {
            throw CommandException(Localization::WSLCCLI_RequiredArgumentError(arg.Name()));
        }

        if ((arg.Limit() > 0) && (arg.Limit() < source.Count(arg.Type())))
        {
            throw CommandException(Localization::WSLCCLI_TooManyArgumentsError(arg.Name()));
        }

        if (source.Contains(arg.Type()))
        {
            arg.Validate(source);
        }
    }

    if (runInternalHook)
    {
        ValidateArgumentsInternal(source);
    }
}

void Command::Execute(CLIExecutionContext& context) const
{
    // If Help was part of the validated argument set, we will output help instead of executing.
    if (context.Args.Contains(ArgType::Help))
    {
        OutputHelp(context.Reporter);
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

std::vector<Argument> Command::GetGlobalsAndEnvArguments() const
{
    auto merged = GetGlobalArguments();
    auto envOnly = GetEnvArguments();

    // Globals listed first, so the loop below treats them as the winners.
    merged.reserve(merged.size() + envOnly.size());
    for (auto& arg : envOnly)
    {
        const auto type = arg.Type();
        const bool alreadyPresent =
            std::any_of(merged.begin(), merged.end(), [type](const Argument& existing) { return existing.Type() == type; });
        if (!alreadyPresent)
        {
            merged.emplace_back(std::move(arg));
        }
    }

    return merged;
}
} // namespace wsl::windows::wslc
