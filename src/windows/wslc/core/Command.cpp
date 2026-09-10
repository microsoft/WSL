/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Command.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "precomp.h"
#include "Argument.h"
#include "Command.h"
#include "CommandLineParser.h"
#include "Invocation.h"
#include "ArgumentParser.h"
#include "TableOutput.h"

#include <algorithm>
#include <typeinfo>

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::common::vt;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {

std::wstring s_ExecutableName = L"wslc";

namespace {
    std::vector<std::wstring> WrapAliases(std::span<const std::wstring> aliases, std::optional<size_t> consoleWidth, size_t indent)
    {
        std::vector<std::wstring> lines;
        std::wstring line(indent, L' ');

        for (size_t i = 0; i < aliases.size(); ++i)
        {
            std::wstring token = aliases[i];
            if (i + 1 < aliases.size())
            {
                token += L',';
            }

            const bool hasAlias = line.size() > indent;
            const size_t requiredWidth = token.size() + (hasAlias ? 1 : 0);
            if (hasAlias && consoleWidth.has_value() && line.size() + requiredWidth > *consoleWidth)
            {
                lines.emplace_back(std::move(line));
                line.assign(indent, L' ');
            }
            else if (hasAlias)
            {
                line += L' ';
            }

            line += token;
        }

        if (line.size() > indent)
        {
            lines.emplace_back(std::move(line));
        }

        return lines;
    }

    void AddCommandInvocations(const Command& command, std::vector<std::wstring>& invocations)
    {
        const auto addInvocation = [&](std::wstring_view name) {
            auto invocation = command.FormatInvocation(name);
            if (std::ranges::find(invocations, invocation) == invocations.end())
            {
                invocations.emplace_back(std::move(invocation));
            }
        };

        addInvocation(command.Name());
        for (const auto alias : command.Aliases())
        {
            addInvocation(alias);
        }
    }

    std::vector<std::wstring> GetCommandInvocations(const Command& command)
    {
        std::vector<std::wstring> invocations;
        std::vector<std::reference_wrapper<const Command>> pending{std::cref(command.Root())};
        while (!pending.empty())
        {
            const auto& current = pending.back().get();
            pending.pop_back();

            if (typeid(command) == typeid(current))
            {
                AddCommandInvocations(current, invocations);
            }

            const auto& children = current.GetCommands();
            for (auto child = children.rbegin(); child != children.rend(); ++child)
            {
                pending.emplace_back(std::cref(**child));
            }
        }

        if (invocations.empty())
        {
            AddCommandInvocations(command, invocations);
        }

        if (invocations.size() == 1)
        {
            invocations.clear();
        }

        return invocations;
    }
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

std::wstring Command::FormatInvocation(std::wstring_view name) const
{
    if (Parent().has_value())
    {
        std::vector<std::wstring_view> commandPath{name};
        auto command = Parent();
        while (command.has_value())
        {
            if (command->get().Parent().has_value() || command->get().FullName().find_first_of(ParentSplitChar) != std::wstring::npos)
            {
                commandPath.emplace_back(command->get().Name());
            }

            command = command->get().Parent();
        }

        std::wstring invocation = s_ExecutableName;
        for (auto command = commandPath.rbegin(); command != commandPath.rend(); ++command)
        {
            invocation += L' ';
            invocation += *command;
        }

        return invocation;
    }

    std::wstring commandChain = FullName();
    const auto firstSplit = commandChain.find_first_of(ParentSplitChar);
    if (firstSplit == std::wstring::npos)
    {
        return s_ExecutableName;
    }

    commandChain = commandChain.substr(firstSplit + 1);
    const auto lastSplit = commandChain.find_last_of(ParentSplitChar);
    commandChain.replace(lastSplit == std::wstring::npos ? 0 : lastSplit + 1, std::wstring::npos, name);
    std::ranges::replace(commandChain, ParentSplitChar, L' ');
    return std::format(L"{} {}", s_ExecutableName, commandChain);
}

const Command& Command::Root() const
{
    std::reference_wrapper<const Command> root = std::cref(*this);
    for (auto parent = Parent(); parent.has_value(); parent = parent->get().Parent())
    {
        root = *parent;
    }

    return root.get();
}

const std::vector<std::unique_ptr<Command>>& Command::GetCommands() const
{
    if (!m_commands.has_value())
    {
        auto commands = CreateCommands();
        for (auto& command : commands)
        {
            THROW_HR_IF(E_UNEXPECTED, !command);
            command->m_parent = std::cref(*this);
            command->m_fullName = m_fullName;
            command->m_fullName += ParentSplitChar;
            command->m_fullName += command->Name();
        }

        m_commands.emplace(std::move(commands));
    }

    return *m_commands;
}

void Command::OutputHelp(Terminal& terminal, HelpOutput output, const CommandException* exception, std::span<const Argument> relevantArguments) const
{
    constexpr size_t c_helpRowIndent = 2;
    constexpr size_t c_helpColumnPadding = 2;
    const bool fullHelp = output == HelpOutput::Full;
    const bool commandHelp = output == HelpOutput::Command;
    const bool argumentHelp = output == HelpOutput::Argument;
    const auto helpLevel = fullHelp ? Terminal::Level::Output : Terminal::Level::Info;

    // Emphasis sequences for help output.
    static const auto& HelpHeadingEmphasis = Format::Bright;
    static const auto& HelpCommandEmphasis = Format::Bright;
    static const auto& HelpArgumentEmphasis = Format::Bright;
    static const auto& HelpMetaEmphasis = Format::Dim;
    static const auto& HelpPlaceholderEmphasis = Format::Fg::BrightCyan;

    if (fullHelp)
    {
        terminal.Write(helpLevel, L"{}{}{}\n\n", HelpMetaEmphasis, Localization::WSLCCLI_CopyrightHeader(), Format::Default);
    }

    // Error if given
    if (exception)
    {
        terminal.Error(L"{}\n\n", exception->Message());
    }

    if (fullHelp)
    {
        terminal.Write(helpLevel, L"{}\n\n", LongDescription());
    }

    const auto commandInvocation = FormatInvocation();

    std::vector<std::wstring> commandAliases;
    if (fullHelp)
    {
        commandAliases = GetCommandInvocations(*this);
    }
    const auto& commands = GetCommands();
    auto arguments = GetAllArguments();
    std::vector<Argument> helpArguments;
    if (fullHelp)
    {
        helpArguments = arguments;
    }
    else if (argumentHelp)
    {
        helpArguments.assign(relevantArguments.begin(), relevantArguments.end());
    }

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

    std::vector<Argument> helpStandardArgs;
    std::vector<Argument> helpPositionalArgs;
    std::vector<Argument> helpForwardArgs;
    for (const auto& arg : helpArguments)
    {
        switch (arg.Kind())
        {
        case Kind::Flag:
        case Kind::Value:
            helpStandardArgs.emplace_back(arg);
            break;
        case Kind::Positional:
            helpPositionalArgs.emplace_back(arg);
            break;
        case Kind::Forward:
            helpForwardArgs.emplace_back(arg);
            break;
        }
    }

    const bool hasHelpArguments = !helpPositionalArgs.empty();
    const bool hasHelpOptions = !helpStandardArgs.empty();
    const bool hasHelpForwardArgs = !helpForwardArgs.empty();

    const auto currentGlobalArguments = GetGlobalArguments();
    const auto globalArgumentScopes = GetGlobalArgumentPath(*this);

    // Build usage line with Write calls for each segment.
    {
        std::wstring usageText = Localization::WSLCCLI_Usage(commandInvocation, L"");

        while (!usageText.empty() && usageText.back() == L' ')
        {
            usageText.pop_back();
        }

        terminal.Write(helpLevel, L"{}{}{}", HelpHeadingEmphasis, usageText, Format::Default);

        if (!currentGlobalArguments.empty())
        {
            terminal.Write(
                helpLevel,
                L" {}[{}{}{}{}]{}",
                HelpMetaEmphasis,
                Format::Default,
                HelpPlaceholderEmphasis,
                Localization::WSLCCLI_GlobalOptions(),
                HelpMetaEmphasis,
                Format::Default);
        }

        if (!commands.empty())
        {
            if (!arguments.empty())
            {
                terminal.Write(helpLevel, L" {}[{}", HelpMetaEmphasis, Format::Default);
            }
            else
            {
                terminal.Write(helpLevel, L" ");
            }

            terminal.Write(
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
                terminal.Write(helpLevel, L"{}]{}", HelpMetaEmphasis, Format::Default);
            }
        }

        if (hasOptions)
        {
            terminal.Write(
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
            terminal.Write(helpLevel, L" ");
            if (!arg.Required())
            {
                terminal.Write(helpLevel, L"{}[{}", HelpMetaEmphasis, Format::Default);
            }

            terminal.Write(
                helpLevel, L"{}<{}{}{}{}{}>{}", HelpMetaEmphasis, Format::Default, HelpPlaceholderEmphasis, arg.Name(), Format::Default, HelpMetaEmphasis, Format::Default);
            if (arg.IsUnlimited())
            {
                terminal.Write(helpLevel, L"{}...{}", HelpMetaEmphasis, Format::Default);
            }

            if (!arg.Required())
            {
                terminal.Write(helpLevel, L"{}]{}", HelpMetaEmphasis, Format::Default);
            }
        }

        if (hasForwardArgs)
        {
            terminal.Write(
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

        terminal.Write(helpLevel, L"\n\n");
    }

    if (fullHelp && !commandAliases.empty())
    {
        terminal.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingAliases(), Format::Default);

        std::optional<size_t> consoleWidth;
        if (const auto width = terminal.GetConsoleWidth(helpLevel); width.has_value() && *width > 0)
        {
            consoleWidth = static_cast<size_t>(*width);
        }

        for (const auto& line : WrapAliases(commandAliases, consoleWidth, c_helpRowIndent))
        {
            terminal.Write(helpLevel, L"{}\n", line);
        }
        terminal.Write(helpLevel, L"\n");
    }

    // Col0: name/command
    // Col1: description (word-wraps at computed column width)
    const auto MakeHelpTable = [&terminal, helpLevel]() -> TableOutput<2> {
        TableOutput<2> table{terminal, {L"", L""}, 50, c_helpColumnPadding, helpLevel};
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
    const auto MakeOptionsTable = [&terminal, helpLevel]() -> TableOutput<3> {
        TableOutput<3> table{terminal, {L"", L"", L""}, {}, 50, c_helpColumnPadding, helpLevel};
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

    const auto AddArgumentRows = [](auto& table, const std::vector<Argument>& args) {
        for (const auto& arg : args)
        {
            FormattedCell aliasCell{L""};
            std::wstring name = arg.Name();
            if (arg.Kind() == Kind::Flag || arg.Kind() == Kind::Value)
            {
                if (!arg.Alias().empty())
                {
                    aliasCell = FormattedCell(std::wstring{WSLC_CLI_ARG_ID_CHAR} + arg.Alias(), HelpArgumentEmphasis);
                }

                name = std::wstring{WSLC_CLI_ARG_ID_CHAR} + std::wstring{WSLC_CLI_ARG_ID_CHAR} + name;
            }

            table.WriteRow({
                std::move(aliasCell),
                FormattedCell(std::move(name), HelpArgumentEmphasis),
                FormattedCell(arg.Description()),
            });
        }
    };

    if ((fullHelp || commandHelp) && !commands.empty())
    {
        terminal.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingCommands(), Format::Default);

        auto table = MakeHelpTable();
        for (const auto& command : commands)
        {
            table.WriteRow({
                FormattedCell(command->Name(), HelpCommandEmphasis),
                FormattedCell(command->ShortDescription()),
            });
        }
        table.Complete();

        if (fullHelp)
        {
            terminal.Write(helpLevel, L"\n{} [{}]\n", Localization::WSLCCLI_HelpForDetails(), WSLC_CLI_HELP_ARG_STRING);
        }
    }

    if (argumentHelp && !helpArguments.empty())
    {
        const bool onlyRelatedOptions = std::ranges::all_of(helpArguments, &Argument::IsOption);

        terminal.Write(
            helpLevel,
            L"{}{}{}\n",
            HelpHeadingEmphasis,
            onlyRelatedOptions ? Localization::WSLCCLI_HeadingRelatedOptions() : Localization::WSLCCLI_HeadingRelatedArguments(),
            Format::Default);

        auto table = MakeOptionsTable();
        AddArgumentRows(table, helpArguments);
        table.Complete();
    }
    else if (fullHelp && !helpArguments.empty())
    {
        if (!commands.empty())
        {
            terminal.Write(helpLevel, L"\n");
        }

        // Arguments table: positional and forward args, name (emphasized) | description
        if (hasHelpArguments || hasHelpForwardArgs)
        {
            terminal.Write(helpLevel, L"{}{}{}\n", HelpHeadingEmphasis, Localization::WSLCCLI_HeadingArguments(), Format::Default);

            auto table = MakeHelpTable();

            for (const auto& arg : helpPositionalArgs)
            {
                table.WriteRow({
                    FormattedCell(arg.Name(), HelpArgumentEmphasis),
                    FormattedCell(arg.Description()),
                });
            }

            for (const auto& arg : helpForwardArgs)
            {
                table.WriteRow({
                    FormattedCell(arg.Name(), HelpArgumentEmphasis),
                    FormattedCell(arg.Description()),
                });
            }

            table.Complete();
        }
    }

    // Options table: alias (emphasized) | long name (emphasized) | description
    // Global options are appended to the same table so column widths are shared.
    if (fullHelp && (hasHelpOptions || !globalArgumentScopes.empty()))
    {
        if (hasHelpArguments || hasHelpForwardArgs)
        {
            terminal.Write(helpLevel, L"\n");
        }
        else if (fullHelp && !commands.empty() && helpArguments.empty())
        {
            terminal.Write(helpLevel, L"\n");
        }

        auto table = MakeOptionsTable();

        if (hasHelpOptions)
        {
            table.WriteLine(FormattedCell(Localization::WSLCCLI_HeadingOptions(), HelpHeadingEmphasis));
            AddArgumentRows(table, helpStandardArgs);
        }

        bool hasPreviousOptionSection = hasHelpOptions;
        for (const auto& scope : globalArgumentScopes)
        {
            if (hasPreviousOptionSection)
            {
                table.WriteLine();
            }

            table.WriteLine(FormattedCell(Localization::WSLCCLI_HeadingScopedGlobalOptions(scope.CommandInvocation), HelpHeadingEmphasis));
            AddArgumentRows(table, scope.Arguments);
            hasPreviousOptionSection = true;
        }

        table.Complete();
    }

    if (!fullHelp)
    {
        if ((commandHelp && !commands.empty()) || (argumentHelp && !helpArguments.empty()))
        {
            terminal.Write(helpLevel, L"\n");
        }

        terminal.Write(helpLevel, L"{}\n", Localization::WSLCCLI_RunHelpForMoreInformation(commandInvocation));
    }
}

std::optional<std::reference_wrapper<const Command>> Command::FindSubCommand(InvocationCursor& invocation) const
{
    auto itr = invocation.begin();
    if (itr == invocation.end() || (*itr)[0] == WSLC_CLI_ARG_ID_CHAR)
    {
        // No more command arguments to check, so no command to find
        return {};
    }

    const auto& commands = GetCommands();
    if (commands.empty())
    {
        return {};
    }

    for (const auto& command : commands)
    {
        if (wsl::shared::string::IsEqual(*itr, command->Name()))
        {
            invocation.AdvancePast(itr);
            return std::cref(*command);
        }

        for (const auto& alias : command->Aliases())
        {
            if (wsl::shared::string::IsEqual(*itr, alias))
            {
                invocation.AdvancePast(itr);
                return std::cref(*command);
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
    InvocationCursor& invocation, ArgMap& target, std::vector<Argument> definedArgs, bool optionsOnly, bool stopOnUnknown, const std::vector<Argument>& overridableDefaults) const
{
    if (definedArgs.empty())
    {
        return;
    }

    ParseArgumentsStateMachine stateMachine{invocation, target, std::move(definedArgs), optionsOnly, stopOnUnknown, overridableDefaults};

    while (stateMachine.Step())
    {
        stateMachine.ThrowIfError();
    }
    stateMachine.ThrowIfError();

    invocation.SetPosition(stateMachine.Position());
}

// Validates the ArgMap produced by ParseArguments. ArgMap is assumed to have
// been populated and parsed successfully from the invocation and now we are validating
// that the arguments provided meet the requirements of the command. This includes checking
// that all required arguments are present. Count limits are enforced during parsing
// (single-value args are last-wins), so they are not re-checked here.
// Any defined validation for specific ArgTypes are also run.
void Command::ValidateArguments(ArgMap& source, const std::vector<Argument>& definedArgs, bool runInternalHook) const
{
    if (source.GetValue<ArgType::Help>())
    {
        return;
    }

    for (const auto& arg : definedArgs)
    {
        if (arg.Required() && !source.Contains(arg.Type()))
        {
            const auto name = arg.IsOption() ? std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + arg.Name() : arg.Name();
            throw ArgumentException(
                arg.IsOption() ? Localization::WSLCCLI_RequiredArgumentOptionError(name)
                               : Localization::WSLCCLI_RequiredArgumentError(arg.Name()),
                arg);
        }

        if (source.Contains(arg.Type()))
        {
            try
            {
                arg.Validate(source);
            }
            catch (const ArgumentException& exception)
            {
                std::vector<Argument> configuredArguments;
                if (exception.Arguments().empty())
                {
                    configuredArguments.emplace_back(arg);
                }
                else
                {
                    configuredArguments.reserve(exception.Arguments().size());
                    for (const auto& exceptionArgument : exception.Arguments())
                    {
                        const auto configuredArgument = std::ranges::find(definedArgs, exceptionArgument.Type(), &Argument::Type);
                        configuredArguments.emplace_back(configuredArgument != definedArgs.end() ? *configuredArgument : exceptionArgument);
                    }
                }

                throw ArgumentException(exception.Message(), std::move(configuredArguments));
            }
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
    if (context.Args.GetValue<ArgType::Help>())
    {
        OutputHelp(context.Terminal);
    }
    else
    {
        // Execute internal has the actual command execution path.
        ExecuteInternal(context);
    }
}

void Command::ValidateArgumentsInternal(ArgMap&) const
{
    // Commands may not need any extra validation; they'll override if they do.
}

std::vector<Argument> Command::GetArgumentsForHelp(std::initializer_list<ArgType> types) const
{
    auto arguments = GetAllArguments();
    const auto globalArgumentScopes = GetGlobalArgumentPath(*this);

    for (const auto& scope : globalArgumentScopes)
    {
        arguments.insert(arguments.end(), scope.Arguments.begin(), scope.Arguments.end());
    }

    std::vector<Argument> result;
    result.reserve(types.size());

    for (const auto type : types)
    {
        const auto argument = std::ranges::find(arguments, type, &Argument::Type);
        THROW_HR_IF_MSG(E_INVALIDARG, argument == arguments.end(), "Argument type %zu is not configured for command", static_cast<size_t>(type));
        result.emplace_back(*argument);
    }

    return result;
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
