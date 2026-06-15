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
#include "TableOutput.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::common::vt;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {

namespace {
    // Leading spaces before the first column of every help table row, matching Docker's style.
    constexpr size_t c_helpRowIndent = 2;

    // Spaces between columns in help tables, matching Docker's style.
    constexpr size_t c_helpColumnPadding = 2;

    // Help output styling. Change here to update all help text appearance.
    const Sequence& HelpCommandEmphasis = Format::Bright;
    const Sequence& HelpArgumentEmphasis = Format::Bright;
    const Sequence& HelpHeadingEmphasis = Format::Bright;
    const Sequence& HelpCopyrightEmphasis = Format::Dim;
    const Sequence& HelpMetaEmphasis = Format::Dim;
    const Sequence& HelpPlaceholderEmphasis = Format::Fg::BrightCyan;
} // namespace

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
    const bool colorEnabled = reporter.IsColorEnabled();

    // Help body routes to stdout on explicit help and stderr (Info) when surfaced
    // as part of a CommandException so the calling shell can separate the diagnostic
    // text from any data on stdout.
    const Reporter::Level helpLevel = exception ? Reporter::Level::Info : Reporter::Level::Output;

    const auto ApplyColor = [colorEnabled](const Sequence& seq, std::wstring text) -> std::wstring {
        if (!colorEnabled)
        {
            return text;
        }
        return seq + text + Format::Default;
    };

    // Copyright header — dimmed so it doesn't compete with the help content.
    reporter.GetWriter(helpLevel) << HelpCopyrightEmphasis << Localization::WSLCCLI_CopyrightHeader() << std::endl << std::endl;

    // Error message in red when a CommandException is supplied.
    if (exception)
    {
        reporter.Error() << exception->Message() << std::endl << std::endl;
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

    // Description and usage line
    reporter.GetWriter(helpLevel) << LongDescription() << std::endl << std::endl;

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

    // Build the usage line: "Usage: wslc <chain> [<command>] [<options>] <positional>..."
    // Usage follows the Microsoft convention:
    // https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/command-line-syntax-key
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

    {
        std::wostringstream usageLine;

        // "Usage: wslc <chain>" — bold as the section label.
        usageLine << ApplyColor(HelpHeadingEmphasis, Localization::WSLCCLI_Usage(s_ExecutableName, std::wstring_view{commandChain}));

        if (!commands.empty())
        {
            usageLine << L' ';
            if (!arguments.empty())
            {
                usageLine << ApplyColor(HelpMetaEmphasis, L"[");
            }
            usageLine << ApplyColor(HelpMetaEmphasis, L"<") << ApplyColor(HelpPlaceholderEmphasis, Localization::WSLCCLI_Command())
                      << ApplyColor(HelpMetaEmphasis, L">");
            if (!arguments.empty())
            {
                usageLine << ApplyColor(HelpMetaEmphasis, L"]");
            }
        }

        if (hasOptions)
        {
            usageLine << L' ' << ApplyColor(HelpMetaEmphasis, L"[<")
                      << ApplyColor(HelpPlaceholderEmphasis, Localization::WSLCCLI_Options())
                      << ApplyColor(HelpMetaEmphasis, L">]");
        }

        for (const auto& arg : positionalArgs)
        {
            usageLine << L' ';
            if (!arg.Required())
            {
                usageLine << ApplyColor(HelpMetaEmphasis, L"[");
            }
            usageLine << ApplyColor(HelpMetaEmphasis, L"<") << ApplyColor(HelpPlaceholderEmphasis, arg.Name())
                      << ApplyColor(HelpMetaEmphasis, L">");
            if (arg.Limit() > 1)
            {
                usageLine << ApplyColor(HelpMetaEmphasis, L"...");
            }
            if (!arg.Required())
            {
                usageLine << ApplyColor(HelpMetaEmphasis, L"]");
            }
        }

        if (hasForwardArgs)
        {
            usageLine << L' ' << ApplyColor(HelpMetaEmphasis, L"[<")
                      << ApplyColor(HelpPlaceholderEmphasis, forwardArgs.front().Name()) << ApplyColor(HelpMetaEmphasis, L">...]");
        }

        reporter.GetWriter(helpLevel) << usageLine.str() << std::endl << std::endl;
    }

    if (!commandAliases.empty())
    {
        reporter.GetWriter(helpLevel) << HelpHeadingEmphasis << Localization::WSLCCLI_HeadingAliases() << std::endl;

        std::wostringstream aliasLine;
        for (size_t i = 0; i < commandAliases.size(); ++i)
        {
            if (i != 0)
            {
                aliasLine << L", ";
            }
            aliasLine << commandAliases[i];
        }
        reporter.GetWriter(helpLevel) << std::wstring(c_helpRowIndent, L' ') << aliasLine.str() << std::endl << std::endl;
    }

    if (!commands.empty())
    {
        reporter.GetWriter(helpLevel) << HelpHeadingEmphasis << Localization::WSLCCLI_HeadingCommands() << std::endl;

        auto table = MakeHelpTable();
        for (const auto& command : commands)
        {
            table.OutputLine({
                ApplyColor(HelpCommandEmphasis, std::wstring{command->Name()}),
                command->ShortDescription(),
            });
        }
        table.Complete();

        reporter.GetWriter(helpLevel) << std::endl
                                      << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L']' << std::endl;
    }

    if (!arguments.empty())
    {
        if (!commands.empty())
        {
            reporter.GetWriter(helpLevel) << std::endl;
        }

        // Arguments table: positional and forward args, name (emphasized) | description
        if (hasArguments || hasForwardArgs)
        {
            reporter.GetWriter(helpLevel) << HelpHeadingEmphasis << Localization::WSLCCLI_HeadingArguments() << std::endl;

            auto table = MakeHelpTable();

            for (const auto& arg : positionalArgs)
            {
                table.OutputLine({
                    ApplyColor(HelpArgumentEmphasis, arg.Name()),
                    arg.Description(),
                });
            }

            for (const auto& arg : forwardArgs)
            {
                table.OutputLine({
                    ApplyColor(HelpArgumentEmphasis, arg.Name()),
                    arg.Description(),
                });
            }

            table.Complete();
        }

        // Options table: alias (emphasized) | long name (emphasized) | description
        if (hasOptions)
        {
            if (hasArguments || hasForwardArgs)
            {
                reporter.GetWriter(helpLevel) << std::endl;
            }

            reporter.GetWriter(helpLevel) << HelpHeadingEmphasis << Localization::WSLCCLI_HeadingOptions() << std::endl;

            auto table = MakeOptionsTable();
            for (const auto& arg : standardArgs)
            {
                // Short alias column: "-f" (emphasized) when an alias exists, empty otherwise.
                std::wstring aliasCell;
                if (!arg.Alias().empty())
                {
                    aliasCell = ApplyColor(HelpArgumentEmphasis, std::wstring{WSLC_CLI_ARG_ID_CHAR} + arg.Alias());
                }

                table.OutputLine({
                    std::move(aliasCell),
                    ApplyColor(HelpArgumentEmphasis, std::wstring{WSLC_CLI_ARG_ID_CHAR} + std::wstring{WSLC_CLI_ARG_ID_CHAR} + arg.Name()),
                    arg.Description(),
                });
            }
            table.Complete();
        }
    }
}

std::unique_ptr<Command> Command::FindSubCommand(Invocation& inv) const
{
    auto itr = inv.begin();
    if (itr == inv.end() || (*itr)[0] == WSLC_CLI_ARG_ID_CHAR)
    {
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

void Command::ParseArguments(Invocation& inv, ArgMap& execArgs) const
{
    auto definedArgs = GetAllArguments();
    ParseArgumentsStateMachine stateMachine{inv, execArgs, std::move(definedArgs)};
    while (stateMachine.Step())
    {
        stateMachine.ThrowIfError();
    }
}

void Command::ValidateArguments(ArgMap& execArgs) const
{
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

        if ((arg.Limit() > 0) && (arg.Limit() < execArgs.Count(arg.Type())))
        {
            throw CommandException(Localization::WSLCCLI_TooManyArgumentsError(arg.Name()));
        }

        if (execArgs.Contains(arg.Type()))
        {
            arg.Validate(execArgs);
        }
    }

    ValidateArgumentsInternal(execArgs);
}

void Command::Execute(CLIExecutionContext& context) const
{
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
} // namespace wsl::windows::wslc
