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

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <span>

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
namespace {
    std::vector<Argument> GetInheritedGlobalArguments(const Command& command)
    {
        std::vector<Argument> arguments;
        for (auto ancestor = command.Parent(); ancestor.has_value(); ancestor = ancestor->get().Parent())
        {
            auto ancestorArguments = ancestor->get().GetScopedArguments(Scope::Global, Flags::None);
            arguments.insert(arguments.end(), ancestorArguments.begin(), ancestorArguments.end());
        }

        return arguments;
    }

    void ValidateArgumentRelationships(const Command& selected, ArgMap& arguments)
    {
        std::vector<std::reference_wrapper<const Command>> commandPath;
        for (auto command = std::optional{std::cref(selected)}; command.has_value(); command = command->get().Parent())
        {
            commandPath.emplace_back(*command);
        }

        for (auto command = commandPath.rbegin(); command != commandPath.rend(); ++command)
        {
            command->get().ValidateArgumentRelationships(arguments);
        }
    }

    void ThrowIfMisplacedGlobalOption(std::wstring_view token, const Command& currentCommand)
    {
        const auto findOption = [token](std::span<const Argument> arguments) -> std::optional<std::reference_wrapper<const Argument>> {
            const auto argument =
                std::ranges::find_if(arguments, [token](const auto& candidate) { return candidate.MatchesOption(token); });
            return argument != arguments.end() ? std::optional{std::cref(*argument)} : std::nullopt;
        };

        const auto commandArguments = currentCommand.GetScopedArguments(Scope::Command, Flags::None);
        const auto globalArguments = currentCommand.GetScopedArguments(Scope::Global, Flags::None);
        if (findOption(commandArguments).has_value() || findOption(globalArguments).has_value())
        {
            return;
        }

        std::reference_wrapper<const Command> nextCommand = std::cref(currentCommand);
        for (auto owner = currentCommand.Parent(); owner.has_value(); owner = owner->get().Parent())
        {
            const auto ownerGlobalArguments = owner->get().GetScopedArguments(Scope::Global, Flags::None);
            if (const auto argument = findOption(ownerGlobalArguments); argument.has_value())
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
        ApplyEnvironmentOptions(context.Args, command.GetScopedArguments(Scope::Global));

        auto globalArguments = command.GetScopedArguments(Scope::Global, Flags::None);
        command.ParseArguments(
            invocation,
            context.Args,
            globalArguments,
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true);
        command.ValidateArguments(context.Args, command.GetScopedArguments(Scope::Global));

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
    ApplyEnvironmentOptions(arguments, Root().GetScopedArguments(Scope::Global));
}

void CommandInvocation::ParseCommandLine(CLIExecutionContext& context)
{
    const auto start = std::chrono::steady_clock::now();
    const auto reportFailure = [&]() {
        WSLC_DEBUG(
            context,
            L"Command-line parsing failed after {} ms.\n",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    };
    const auto enableDebugOutput = [&]() {
        if (context.Args.Contains(ArgType::Debug))
        {
            context.Terminal.SetDebugEnabled(context.Args.GetValue<ArgType::Debug>());
        }
    };

    std::optional<std::reference_wrapper<const Command>> subcommand;
    try
    {
        subcommand = ParseGlobalArgumentsAndFindSubcommand(m_cursor, context, Selected());
    }
    catch (...)
    {
        enableDebugOutput();
        reportFailure();
        throw;
    }

    enableDebugOutput();
    try
    {
        while (subcommand)
        {
            Select(subcommand->get());
            subcommand = ParseGlobalArgumentsAndFindSubcommand(m_cursor, context, Selected());
        }

        ApplyEnvironmentOptions(context.Args, Selected().GetScopedArguments(Scope::Command));
        try
        {
            Selected().ParseArguments(
                m_cursor,
                context.Args,
                Selected().GetScopedArguments(Scope::Command, Flags::None),
                /*optionsOnly*/ false,
                /*stopOnUnknown*/ false,
                GetInheritedGlobalArguments(Selected()));
        }
        catch (const ArgumentException& exception)
        {
            if (exception.UnknownOptionToken().has_value())
            {
                ThrowIfMisplacedGlobalOption(*exception.UnknownOptionToken(), Selected());
            }

            throw;
        }

        Selected().ValidateArguments(context.Args, Selected().GetScopedArguments(Scope::Command));
        if (!context.Args.GetValue<ArgType::Help>())
        {
            ValidateArgumentRelationships(Selected(), context.Args);
        }
    }
    catch (...)
    {
        reportFailure();
        throw;
    }

    WSLC_DEBUG(context, L"Selected command: {}\n", Selected().FormatInvocation());
    WSLC_DEBUG(
        context,
        L"Command-line parsing completed in {} ms.\n",
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

void CommandInvocation::Execute(CLIExecutionContext& context) const
{
    WSLC_DEBUG(context, L"Executing command: {}\n", Selected().FormatInvocation());
    const auto start = std::chrono::steady_clock::now();

    try
    {
        Selected().Execute(context);
    }
    catch (...)
    {
        WSLC_DEBUG(
            context,
            L"Command execution failed after {} ms.\n",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        throw;
    }
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
