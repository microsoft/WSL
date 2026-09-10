/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineParser.cpp

Abstract:

    Implementation of command-line resolution and scoped global option handling.

--*/
#include "precomp.h"
#include "CommandLineParser.h"

#include "CLIExecutionContext.h"
#include "Command.h"
#include "EnvironmentOptions.h"
#include "Invocation.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <span>

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
namespace {
    GlobalArgumentScope MakeGlobalArgumentScope(const Command& command, std::vector<Argument> arguments)
    {
        return {
            .CommandInvocation = command.FormatInvocation(),
            .Arguments = std::move(arguments),
        };
    }

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
                std::wstring optionName{L"--"};
                optionName += argument->get().Name();
                throw ArgumentException(
                    Localization::WSLCCLI_MisplacedInheritedGlobalOptionError(
                        optionName, owner->get().FormatInvocation(), nextCommand.get().Name()),
                    argument->get());
            }

            nextCommand = *owner;
        }
    }

    std::optional<std::reference_wrapper<const Command>> ParseGlobalArgumentsAndFindSubcommand(
        InvocationCursor& invocation, CLIExecutionContext& context, const Command& command, bool applyEnvironmentOptions)
    {
        const auto globalAndEnvironmentArguments = command.GetGlobalsAndEnvArguments();
        if (applyEnvironmentOptions)
        {
            ApplyEnvironmentOptions(context.GlobalArgs, globalAndEnvironmentArguments);
        }

        auto globalArguments = command.GetGlobalArguments();
        command.ParseArguments(
            invocation,
            context.GlobalArgs,
            globalArguments,
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true,
            /*overridableDefaults*/ globalAndEnvironmentArguments);
        command.ValidateArguments(context.GlobalArgs, globalAndEnvironmentArguments, /*runInternalHook*/ false);

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

CommandTree::CommandTree(std::unique_ptr<Command> root) : m_root(std::move(root))
{
    THROW_HR_IF(E_INVALIDARG, !m_root);
}

CommandTree::~CommandTree() = default;
CommandTree::CommandTree(CommandTree&&) noexcept = default;
CommandTree& CommandTree::operator=(CommandTree&&) noexcept = default;

const Command& CommandTree::Root() const
{
    THROW_HR_IF(E_ILLEGAL_METHOD_CALL, !m_root);
    return *m_root;
}

CommandInvocation::CommandInvocation(std::unique_ptr<Command> root, std::vector<std::wstring>&& arguments) :
    m_commands(std::move(root)), m_cursor(std::move(arguments)), m_selected(std::cref(m_commands.Root()))
{
}

CommandInvocation::~CommandInvocation() = default;

CommandInvocation::CommandInvocation(CommandInvocation&& other) noexcept :
    m_commands(std::move(other.m_commands)), m_cursor(std::move(other.m_cursor)), m_selected(other.m_selected)
{
    other.m_selected.reset();
}

CommandInvocation& CommandInvocation::operator=(CommandInvocation&& other) noexcept
{
    if (this != &other)
    {
        m_commands = std::move(other.m_commands);
        m_cursor = std::move(other.m_cursor);
        m_selected = other.m_selected;
        other.m_selected.reset();
    }

    return *this;
}

const Command& CommandInvocation::Root() const
{
    return m_commands.Root();
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

InvocationCursor& CommandInvocation::Cursor() noexcept
{
    return m_cursor;
}

void CommandInvocation::Select(const Command& command)
{
    const auto parent = command.Parent();
    THROW_HR_IF(E_INVALIDARG, !parent.has_value() || &parent->get() != &Selected());
    m_selected = std::cref(command);
}

std::vector<GlobalArgumentScope> GetGlobalArgumentPath(const Command& target)
{
    std::vector<GlobalArgumentScope> path;
    for (auto command = std::optional{std::cref(target)}; command.has_value(); command = command->get().Parent())
    {
        auto globalArguments = command->get().GetGlobalArguments();
        if (!globalArguments.empty())
        {
            path.emplace_back(MakeGlobalArgumentScope(command->get(), std::move(globalArguments)));
        }
    }

    std::ranges::reverse(path);
    return path;
}

void ParseCommandLine(CommandInvocation& invocation, CLIExecutionContext& context, bool applyEnvironmentOptions)
{
    auto subcommand = ParseGlobalArgumentsAndFindSubcommand(invocation.Cursor(), context, invocation.Selected(), applyEnvironmentOptions);
    while (subcommand)
    {
        invocation.Select(subcommand->get());
        subcommand = ParseGlobalArgumentsAndFindSubcommand(invocation.Cursor(), context, invocation.Selected(), applyEnvironmentOptions);
    }

    try
    {
        invocation.Selected().ParseArguments(invocation.Cursor(), context.Args);
    }
    catch (const ArgumentException& exception)
    {
        if (exception.UnknownOptionToken().has_value())
        {
            ThrowIfMisplacedGlobalOption(*exception.UnknownOptionToken(), invocation.Selected());
        }

        throw;
    }

    invocation.Selected().ValidateArguments(context.Args);
}
} // namespace wsl::windows::wslc
