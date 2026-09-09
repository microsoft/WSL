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
#include <format>
#include <functional>
#include <optional>
#include <span>

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
namespace {
    std::wstring FormatCommandInvocation(std::wstring_view fullName)
    {
        const auto firstSplit = fullName.find_first_of(Command::ParentSplitChar);
        if (firstSplit == std::wstring_view::npos)
        {
            return s_ExecutableName;
        }

        std::wstring commandChain{fullName.substr(firstSplit + 1)};
        std::ranges::replace(commandChain, Command::ParentSplitChar, L' ');
        return std::format(L"{} {}", s_ExecutableName, commandChain);
    }

    GlobalArgumentScope MakeGlobalArgumentScope(const Command& command)
    {
        return {
            .CommandFullName = command.FullName(),
            .CommandInvocation = FormatCommandInvocation(command.FullName()),
            .Arguments = command.GetGlobalArguments(),
        };
    }

    void CollectGlobalArgumentScopes(const Command& command, std::vector<GlobalArgumentScope>& scopes)
    {
        auto globalArguments = command.GetGlobalArguments();
        if (!globalArguments.empty())
        {
            scopes.emplace_back(GlobalArgumentScope{
                .CommandFullName = command.FullName(),
                .CommandInvocation = FormatCommandInvocation(command.FullName()),
                .Arguments = std::move(globalArguments),
            });
        }

        for (const auto& subcommand : command.GetCommands())
        {
            CollectGlobalArgumentScopes(*subcommand, scopes);
        }
    }

    bool CollectGlobalArgumentPath(const Command& command, std::wstring_view targetFullName, std::vector<GlobalArgumentScope>& path)
    {
        const bool addedScope = !command.GetGlobalArguments().empty();
        if (addedScope)
        {
            path.emplace_back(MakeGlobalArgumentScope(command));
        }

        if (command.FullName() == targetFullName)
        {
            return true;
        }

        for (const auto& subcommand : command.GetCommands())
        {
            if (CollectGlobalArgumentPath(*subcommand, targetFullName, path))
            {
                return true;
            }
        }

        if (addedScope)
        {
            path.pop_back();
        }

        return false;
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

    struct GlobalArgumentMatch
    {
        std::reference_wrapper<const GlobalArgumentScope> Scope;
        std::reference_wrapper<const Argument> Argument;
    };

    std::optional<std::wstring_view> GetNextCommandName(const GlobalArgumentScope& scope, const Command& currentCommand)
    {
        const auto& scopeName = scope.CommandFullName;
        const auto& currentName = currentCommand.FullName();
        if (currentName.length() <= scopeName.length() || !currentName.starts_with(scopeName) ||
            currentName[scopeName.length()] != Command::ParentSplitChar)
        {
            return std::nullopt;
        }

        const auto nameStart = scopeName.length() + 1;
        const auto nameEnd = currentName.find(Command::ParentSplitChar, nameStart);
        return std::wstring_view{currentName}.substr(nameStart, nameEnd - nameStart);
    }

    void ThrowIfMisplacedGlobalOption(std::wstring_view token, const Command& currentCommand, std::span<const GlobalArgumentScope> globalScopes)
    {
        const auto commandArguments = currentCommand.GetAllArguments();
        if (FindOption(token, commandArguments).has_value())
        {
            return;
        }

        std::vector<GlobalArgumentMatch> matches;
        for (const auto& scope : globalScopes)
        {
            if (scope.CommandFullName == currentCommand.FullName())
            {
                continue;
            }

            if (const auto argument = FindOption(token, scope.Arguments); argument.has_value())
            {
                matches.emplace_back(GlobalArgumentMatch{
                    .Scope = std::cref(scope),
                    .Argument = std::cref(argument->get()),
                });
            }
        }

        if (matches.empty())
        {
            return;
        }

        const auto& firstMatch = matches.front();
        const auto optionName = std::format(L"--{}", firstMatch.Argument.get().Name());
        if (std::ranges::all_of(matches, [&](const auto& match) {
                return match.Scope.get().CommandFullName == firstMatch.Scope.get().CommandFullName;
            }))
        {
            if (const auto nextCommand = GetNextCommandName(firstMatch.Scope.get(), currentCommand))
            {
                throw ArgumentException(
                    Localization::WSLCCLI_MisplacedInheritedGlobalOptionError(optionName, firstMatch.Scope.get().CommandInvocation, *nextCommand),
                    firstMatch.Argument.get());
            }

            throw ArgumentException(
                Localization::WSLCCLI_MisplacedGlobalOptionError(optionName, firstMatch.Scope.get().CommandInvocation),
                firstMatch.Argument.get());
        }

        std::wstring commandScopes;
        for (const auto& match : matches)
        {
            if (!commandScopes.empty())
            {
                commandScopes += L", ";
            }

            commandScopes += std::format(L"'{}'", match.Scope.get().CommandInvocation);
        }

        throw ArgumentException(Localization::WSLCCLI_MisplacedGlobalOptionMultipleScopesError(optionName, commandScopes));
    }

    std::unique_ptr<Command> ParseGlobalArgumentsAndFindSubcommand(
        Invocation& invocation, CLIExecutionContext& context, const Command& command, std::span<const GlobalArgumentScope> globalScopes, bool applyEnvironmentOptions)
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
            if (currentArgument != invocation.end())
            {
                ThrowIfMisplacedGlobalOption(*currentArgument, command, globalScopes);
            }
        }

        return subcommand;
    }
} // namespace

std::vector<GlobalArgumentScope> GetGlobalArgumentScopes(const Command& root)
{
    std::vector<GlobalArgumentScope> scopes;
    CollectGlobalArgumentScopes(root, scopes);
    return scopes;
}

std::vector<GlobalArgumentScope> GetGlobalArgumentPath(const Command& root, std::wstring_view commandFullName)
{
    std::vector<GlobalArgumentScope> path;
    if (!CollectGlobalArgumentPath(root, commandFullName, path))
    {
        path.clear();
    }

    return path;
}

void ParseCommandLine(Invocation& invocation, CLIExecutionContext& context, std::unique_ptr<Command>& command, bool applyEnvironmentOptions)
{
    const auto globalScopes = GetGlobalArgumentScopes(*command);

    auto subcommand = ParseGlobalArgumentsAndFindSubcommand(invocation, context, *command, globalScopes, applyEnvironmentOptions);
    while (subcommand)
    {
        command = std::move(subcommand);
        subcommand = ParseGlobalArgumentsAndFindSubcommand(invocation, context, *command, globalScopes, applyEnvironmentOptions);
    }

    try
    {
        command->ParseArguments(invocation, context.Args);
    }
    catch (const ArgumentException& exception)
    {
        if (exception.UnknownOptionToken().has_value())
        {
            ThrowIfMisplacedGlobalOption(*exception.UnknownOptionToken(), *command, globalScopes);
        }

        throw;
    }

    command->ValidateArguments(context.Args);
}
} // namespace wsl::windows::wslc
