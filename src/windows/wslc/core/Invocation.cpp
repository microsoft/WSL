/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Invocation.cpp

Abstract:

    Implementation of command invocation parsing and execution.

--*/
#include "precomp.h"
#include "Invocation.h"

#include "CLIExecutionContext.h"
#include "Command.h"
#include "EnvironmentOptions.h"

#include <functional>
#include <optional>
#include <span>

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
namespace {
    std::optional<std::reference_wrapper<const Argument>> FindOption(std::wstring_view token, std::span<const Argument> arguments)
    {
        if (token.length() < 2 || token.front() != WSLC_CLI_ARG_ID_CHAR)
        {
            return std::nullopt;
        }

        const bool longName = token[1] == WSLC_CLI_ARG_ID_CHAR;
        const auto optionStart = longName ? token.find_first_not_of(WSLC_CLI_ARG_ID_CHAR) : 1;
        if (optionStart == std::wstring_view::npos)
        {
            return std::nullopt;
        }

        auto optionName = token.substr(optionStart);
        if (const auto separator = optionName.find_first_of(WSLC_CLI_ARG_SPLIT_CHAR); separator != std::wstring_view::npos)
        {
            optionName = optionName.substr(0, separator);
        }

        for (const auto& argument : arguments)
        {
            if (!argument.IsOption())
            {
                continue;
            }

            const auto& configuredName = longName ? argument.Name() : argument.Alias();
            if (!configuredName.empty() && wsl::shared::string::IsEqual(optionName, configuredName))
            {
                return std::cref(argument);
            }
        }

        return std::nullopt;
    }

    void ThrowIfMisplacedGlobalOption(std::wstring_view token, const Command& currentCommand)
    {
        const auto commandArguments = currentCommand.GetAllArguments();
        if (FindOption(token, commandArguments).has_value())
        {
            return;
        }

        std::reference_wrapper<const Command> nextCommand = std::cref(currentCommand);
        for (auto owner = currentCommand.Parent(); owner.has_value(); owner = owner->get().Parent())
        {
            const auto globalArguments = owner->get().GetGlobalArguments();
            if (const auto argument = FindOption(token, globalArguments); argument.has_value())
            {
                const auto& globalOwner = argument->get().GlobalOwner();
                THROW_HR_IF(E_UNEXPECTED, !globalOwner.has_value());

                std::wstring optionName{L"--"};
                optionName += argument->get().Name();
                throw ArgumentException(
                    Localization::WSLCCLI_MisplacedInheritedGlobalOptionError(
                        optionName, globalOwner->get().FormatInvocation(), nextCommand.get().Name()),
                    argument->get());
            }

            nextCommand = *owner;
        }
    }

    std::optional<std::reference_wrapper<const Command>> ParseGlobalArgumentsAndFindSubcommand(
        InvocationCursor& invocation, CLIExecutionContext& context, const Command& command)
    {
        const auto globalAndEnvironmentArguments = command.GetArgumentsAndEnvironment(ArgumentScope::Global);
        ApplyEnvironmentOptions(context.Args, globalAndEnvironmentArguments);

        auto globalArguments = command.GetGlobalArguments();
        command.ParseArguments(
            invocation,
            context.Args,
            globalArguments,
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true,
            /*overridableDefaults*/ globalAndEnvironmentArguments);
        command.ValidateArguments(context.Args, globalAndEnvironmentArguments, /*runInternalHook*/ false);

        auto subcommand = command.FindSubCommand(invocation);
        if (!subcommand)
        {
            const auto currentArgument = invocation.begin();
            if (currentArgument != invocation.end() && !currentArgument->empty() && currentArgument->front() == WSLC_CLI_ARG_ID_CHAR)
            {
                ThrowIfMisplacedGlobalOption(*currentArgument, command);
            }
        }

        return subcommand;
    }
} // namespace

CommandInvocation::CommandInvocation(std::unique_ptr<Command> root, std::vector<std::wstring>&& arguments) :
    m_root(std::move(root)), m_cursor(std::move(arguments))
{
    THROW_HR_IF(E_INVALIDARG, !m_root);
    m_selected = std::cref(*m_root);
}

CommandInvocation::~CommandInvocation() = default;

CommandInvocation::CommandInvocation(CommandInvocation&& other) noexcept :
    m_root(std::move(other.m_root)), m_cursor(std::move(other.m_cursor)), m_selected(other.m_selected)
{
    other.m_selected.reset();
}

CommandInvocation& CommandInvocation::operator=(CommandInvocation&& other) noexcept
{
    if (this != &other)
    {
        m_root = std::move(other.m_root);
        m_cursor = std::move(other.m_cursor);
        m_selected = other.m_selected;
        other.m_selected.reset();
    }

    return *this;
}

const Command& CommandInvocation::Root() const
{
    THROW_HR_IF(E_ILLEGAL_METHOD_CALL, !m_root);
    return *m_root;
}

const Command& CommandInvocation::Selected() const
{
    THROW_HR_IF(E_ILLEGAL_METHOD_CALL, !m_selected.has_value());
    return m_selected.value().get();
}

const std::vector<std::wstring>& CommandInvocation::OriginalArguments() const noexcept
{
    return m_cursor.OriginalArguments();
}

size_t CommandInvocation::Position() const noexcept
{
    return m_cursor.Position();
}

void CommandInvocation::ApplyRootEnvironmentOptions(argument::ArgMap& arguments) const
{
    ApplyEnvironmentOptions(arguments, Root().GetEnvArguments());
}

void CommandInvocation::ParseCommandLine(CLIExecutionContext& context)
{
    auto subcommand = ParseGlobalArgumentsAndFindSubcommand(m_cursor, context, Selected());
    while (subcommand)
    {
        Select(subcommand->get());
        subcommand = ParseGlobalArgumentsAndFindSubcommand(m_cursor, context, Selected());
    }

    try
    {
        const auto commandAndEnvironmentArguments = Selected().GetArgumentsAndEnvironment(ArgumentScope::Command);
        ApplyEnvironmentOptions(context.Args, commandAndEnvironmentArguments);
        Selected().ParseArguments(
            m_cursor,
            context.Args,
            Selected().GetCommandArguments(),
            /*optionsOnly*/ false,
            /*stopOnUnknown*/ false,
            /*overridableDefaults*/ commandAndEnvironmentArguments);
    }
    catch (const ArgumentException& exception)
    {
        if (exception.UnknownOptionToken().has_value())
        {
            ThrowIfMisplacedGlobalOption(*exception.UnknownOptionToken(), Selected());
        }

        throw;
    }

    Selected().ValidateArguments(context.Args, Selected().GetArgumentsAndEnvironment(ArgumentScope::Command), /*runInternalHook*/ true);
}

void CommandInvocation::Execute(CLIExecutionContext& context) const
{
    Selected().Execute(context);
}

void CommandInvocation::OutputHelp(Terminal& terminal, HelpOutput output, const CommandException* exception, std::span<const Argument> relevantArguments) const
{
    Selected().OutputHelp(terminal, output, exception, relevantArguments);
}

void CommandInvocation::Select(const Command& command)
{
    const auto parent = command.Parent();
    THROW_HR_IF(E_INVALIDARG, !parent.has_value() || &parent->get() != &Selected());
    m_selected = std::cref(command);
}
} // namespace wsl::windows::wslc
