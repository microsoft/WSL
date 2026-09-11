/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLICommandUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI Command classes.

--*/

#include "precomp.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Command.h"
#include "RootCommand.h"
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "InspectCommand.h"
#include "NetworkCommand.h"
#include "SessionCommand.h"
#include "SystemCommand.h"
#include "VersionCommand.h"
#include "VolumeCommand.h"
#include "EnvironmentOptions.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLICommandUnitTests {
class WSLCCLICommandUnitTests
{
    WSLC_TEST_CLASS(WSLCCLICommandUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        Log::Comment(L"WSLC CLI Command Unit Tests - Class Setup");
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        Log::Comment(L"WSLC CLI Command Unit Tests - Class Cleanup");
        return true;
    }

    // Test: Verify RootCommand has subcommands
    TEST_METHOD(RootCommand_HasSubcommands)
    {
        auto cmd = RootCommand();

        const auto& subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"RootCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Verify each subcommand is valid
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    TEST_METHOD(RootCommand_RetainsSubcommands)
    {
        RootCommand root;
        const auto& first = root.GetCommands();
        const auto& second = root.GetCommands();

        VERIFY_ARE_EQUAL(first.size(), second.size());
        VERIFY_ARE_EQUAL(first.front().get(), second.front().get());
        VERIFY_IS_TRUE(first.front()->Parent().has_value());
        VERIFY_ARE_EQUAL(&root, &first.front()->Parent()->get());
    }

    // Test: Verify SystemCommand has subcommands
    TEST_METHOD(SystemCommand_HasSubcommands)
    {
        auto cmd = SystemCommand(L"system");
        const auto& subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"SystemCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    // Test: Verify SessionCommand has subcommands (now under system)
    TEST_METHOD(SessionCommand_HasSubcommands)
    {
        auto cmd = SessionCommand(L"system session");
        const auto& subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"SessionCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Log subcommand types
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    // Test: Every prune command binds -f to --force and leaves --filter unaliased. Aliases resolve
    // by first match with no collision detection, so an aliased --filter here would shadow -f.
    TEST_METHOD(PruneCommands_BindShortFToForce)
    {
        const auto verifyPruneArguments = [](const std::vector<Argument>& args, const std::wstring& command) {
            LogComment(L"Verifying prune argument aliases for: " + command);

            const auto find = [&args](ArgType type) -> const Argument* {
                const auto itr = std::find_if(args.begin(), args.end(), [type](const auto& arg) { return arg.Type() == type; });
                return itr == args.end() ? nullptr : &*itr;
            };

            const auto* force = find(ArgType::Force);
            VERIFY_IS_NOT_NULL(force);
            VERIFY_ARE_EQUAL(std::wstring{L"force"}, force->Name());
            VERIFY_ARE_EQUAL(std::wstring{L"f"}, force->Alias());

            const auto* filter = find(ArgType::Filter);
            VERIFY_IS_NOT_NULL(filter);
            VERIFY_ARE_EQUAL(std::wstring{L"filter"}, filter->Name());
            VERIFY_ARE_EQUAL(std::wstring{L""}, filter->Alias());
        };

        verifyPruneArguments(ContainerPruneCommand(L"container").GetArguments(), L"container prune");
        verifyPruneArguments(ImagePruneCommand(L"image").GetArguments(), L"image prune");
        verifyPruneArguments(VolumePruneCommand(L"volume").GetArguments(), L"volume prune");
        verifyPruneArguments(NetworkPruneCommand(L"network").GetArguments(), L"network prune");
    }

    // Test: List commands keep -f bound to --filter, which is why only prune commands were realigned.
    TEST_METHOD(ListCommands_KeepShortFOnFilter)
    {
        const auto verifyFilterAlias = [](const std::vector<Argument>& args, const std::wstring& command) {
            LogComment(L"Verifying filter alias for: " + command);

            const auto itr = std::find_if(args.begin(), args.end(), [](const auto& arg) { return arg.Type() == ArgType::Filter; });
            VERIFY_IS_TRUE(itr != args.end());
            VERIFY_ARE_EQUAL(std::wstring{L"f"}, itr->Alias());
        };

        verifyFilterAlias(ContainerListCommand(L"container").GetArguments(), L"container list");
        verifyFilterAlias(ImageListCommand(L"image").GetArguments(), L"image list");
        verifyFilterAlias(VolumeListCommand(L"volume").GetArguments(), L"volume list");
        verifyFilterAlias(NetworkListCommand(L"network").GetArguments(), L"network list");
    }

    // Test: Verify SessionEnterCommand has the expected arguments
    TEST_METHOD(SessionEnterCommand_HasExpectedArguments)
    {
        auto cmd = SessionEnterCommand(L"system session");
        auto args = cmd.GetArguments();

        // Should have 2 arguments: storage-path (positional, required) and name (value, optional)
        VERIFY_ARE_EQUAL(2u, args.size());

        // Verify storage-path argument
        auto& storagePath = args[0];
        VERIFY_ARE_EQUAL(ArgType::StoragePath, storagePath.Type());
        VERIFY_ARE_EQUAL(Kind::Positional, storagePath.Kind());
        VERIFY_IS_TRUE(storagePath.Required());

        // Verify name argument
        auto& name = args[1];
        VERIFY_ARE_EQUAL(ArgType::Name, name.Type());
        VERIFY_ARE_EQUAL(Kind::Value, name.Kind());
        VERIFY_IS_FALSE(name.Required());
    }

    // Test: Verify SessionEnterCommand descriptions are not empty
    TEST_METHOD(SessionEnterCommand_HasDescriptions)
    {
        auto cmd = SessionEnterCommand(L"system session");

        VERIFY_IS_FALSE(cmd.ShortDescription().empty());
        VERIFY_IS_FALSE(cmd.LongDescription().empty());
    }

    // Test: Verify ContainerCommand has subcommands
    TEST_METHOD(ContainerCommand_HasSubcommands)
    {
        auto cmd = ContainerCommand(L"container");
        const auto& subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"ContainerCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Log subcommand types
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    // Test: Verify image list exposes --all/-a on both the subcommand and root 'images' spelling
    TEST_METHOD(ImageListCommand_HasAllArgument)
    {
        const std::pair<std::wstring, std::vector<Argument>> spellings[] = {
            {L"image list", ImageListCommand(L"image").GetArguments()}, {L"images", ImageListCommand(L"wslc", true).GetArguments()}};

        for (const auto& [label, args] : spellings)
        {
            LogComment(L"Verifying --all for: " + label);

            auto itr = std::find_if(args.begin(), args.end(), [](const Argument& arg) { return arg.Type() == ArgType::All; });

            VERIFY_IS_TRUE(itr != args.end());
            VERIFY_ARE_EQUAL(std::wstring{L"all"}, itr->Name());
            VERIFY_ARE_EQUAL(std::wstring{L"a"}, itr->Alias());
            VERIFY_ARE_EQUAL(Kind::Flag, itr->Kind());
            VERIFY_IS_FALSE(itr->Required());
        }
    }

    // Test: Verify VersionCommand has the correct name
    TEST_METHOD(VersionCommand_HasCorrectName)
    {
        auto cmd = VersionCommand(L"wslc");
        VERIFY_ARE_EQUAL(std::wstring_view(L"version"), cmd.Name());
    }

    // Test: Verify VersionCommand has no subcommands
    TEST_METHOD(VersionCommand_HasNoSubcommands)
    {
        auto cmd = VersionCommand(L"wslc");
        VERIFY_ARE_EQUAL(0u, cmd.GetCommands().size());
    }

    // Test: Verify VersionCommand exposes the --format argument (plus the auto-added --help)
    TEST_METHOD(VersionCommand_HasFormatArgument)
    {
        auto cmd = VersionCommand(L"wslc");

        auto args = cmd.GetArguments();
        VERIFY_ARE_EQUAL(1u, args.size());

        const auto& format = args[0];
        VERIFY_ARE_EQUAL(ArgType::Format, format.Type());
        VERIFY_ARE_EQUAL(Kind::Value, format.Kind());
        VERIFY_IS_FALSE(format.Required());

        VERIFY_ARE_EQUAL(2u, cmd.GetScopedArguments(Scope::Command, Flags::None).size());
    }

    // Test: Verify RootCommand contains VersionCommand as a subcommand
    TEST_METHOD(RootCommand_ContainsVersionCommand)
    {
        auto root = RootCommand();
        const auto& subcommands = root.GetCommands();

        bool found = false;
        for (const auto& subcmd : subcommands)
        {
            if (subcmd->Name() == VersionCommand::CommandName)
            {
                found = true;
                break;
            }
        }

        VERIFY_IS_TRUE(found, L"RootCommand should contain VersionCommand");
    }

    // Test: Verify SystemInfoCommand has the correct name
    TEST_METHOD(SystemInfoCommand_HasCorrectName)
    {
        auto cmd = SystemInfoCommand(L"system");
        VERIFY_ARE_EQUAL(std::wstring_view(L"info"), cmd.Name());
    }

    // Test: Verify SystemInfoCommand has no subcommands
    TEST_METHOD(SystemInfoCommand_HasNoSubcommands)
    {
        auto cmd = SystemInfoCommand(L"system");
        VERIFY_ARE_EQUAL(0u, cmd.GetCommands().size());
    }

    // Test: Verify SystemInfoCommand exposes the --format argument (plus the auto-added --help)
    TEST_METHOD(SystemInfoCommand_HasFormatArgument)
    {
        auto cmd = SystemInfoCommand(L"system");

        auto args = cmd.GetArguments();
        VERIFY_ARE_EQUAL(1u, args.size());

        const auto& format = args[0];
        VERIFY_ARE_EQUAL(ArgType::Format, format.Type());
        VERIFY_ARE_EQUAL(Kind::Value, format.Kind());
        VERIFY_IS_FALSE(format.Required());

        VERIFY_ARE_EQUAL(2u, cmd.GetScopedArguments(Scope::Command, Flags::None).size());
    }

    // Test: Verify SystemCommand contains SystemInfoCommand as a subcommand
    TEST_METHOD(SystemCommand_ContainsSystemInfoCommand)
    {
        auto cmd = SystemCommand(L"system");
        const auto& subcommands = cmd.GetCommands();

        bool found = false;
        for (const auto& subcmd : subcommands)
        {
            if (subcmd->Name() == SystemInfoCommand::CommandName)
            {
                found = true;
                break;
            }
        }

        VERIFY_IS_TRUE(found, L"SystemCommand should contain SystemInfoCommand");
    }

    // SystemInfoCommand is registered twice so that both `wslc system info` and the
    // `wslc info` alias resolve; this pins the second registration.
    TEST_METHOD(RootCommand_ContainsSystemInfoCommand)
    {
        auto root = RootCommand();
        const auto& subcommands = root.GetCommands();

        bool found = false;
        for (const auto& subcmd : subcommands)
        {
            if (subcmd->Name() == SystemInfoCommand::CommandName)
            {
                found = true;
                break;
            }
        }

        VERIFY_IS_TRUE(found, L"RootCommand should contain SystemInfoCommand");
    }

    TEST_METHOD(RootCommand_GlobalCommandLineArguments_OnlySession)
    {
        auto root = RootCommand();
        auto globals = root.GetScopedArguments(Scope::Global, Flags::None);

        VERIFY_ARE_EQUAL(1u, globals.size());
        VERIFY_ARE_EQUAL(ArgType::Session, globals[0].Type());
        VERIFY_ARE_EQUAL(Kind::Value, globals[0].Kind());
    }

    TEST_METHOD(RootCommand_GlobalArguments_HaveExpectedRestrictions)
    {
        auto root = RootCommand();
        const auto arguments = root.GetGlobalArguments();
        const auto allGlobalArguments = root.GetScopedArguments(Scope::Global);
        const auto environmentArguments = root.GetScopedArguments(Scope::Global, Flags::EnvironmentOnly);
        const auto session = std::ranges::find(arguments, ArgType::Session, &Argument::Type);
        const auto noColor = std::ranges::find(arguments, ArgType::NoColor, &Argument::Type);

        VERIFY_ARE_EQUAL(2u, arguments.size());
        VERIFY_ARE_EQUAL(2u, allGlobalArguments.size());
        VERIFY_ARE_EQUAL(1u, environmentArguments.size());
        VERIFY_ARE_EQUAL(ArgType::NoColor, environmentArguments[0].Type());
        VERIFY_IS_TRUE(session != arguments.end());
        VERIFY_ARE_EQUAL(Flags::None, session->Flags());
        VERIFY_IS_TRUE(session->HasAllFlags(Flags::None));
        VERIFY_IS_FALSE(session->HasAnyFlag(Flags::All));
        VERIFY_IS_TRUE(noColor != arguments.end());
        VERIFY_ARE_EQUAL(Flags::EnvironmentOnly, noColor->Flags());
        VERIFY_IS_TRUE(noColor->HasAllFlags(Flags::EnvironmentOnly));
        VERIFY_IS_TRUE(noColor->HasAnyFlag(Flags::All));
        VERIFY_IS_FALSE(noColor->HasAllFlags(Flags::All));
        VERIFY_ARE_EQUAL(Scope::Global, noColor->Scope());
        VERIFY_IS_TRUE(noColor->GlobalOwner().has_value());
        VERIFY_ARE_EQUAL(&root, &noColor->GlobalOwner()->get());

        const auto localArguments = root.GetScopedArguments(Scope::Command);
        VERIFY_IS_TRUE(
            std::ranges::none_of(localArguments, [](const auto& argument) { return argument.HasAnyFlag(Flags::EnvironmentOnly); }));
    }

    // --all-tags is exposed with the -a short alias.
    TEST_METHOD(ImagePushCommand_HasAllTagsArgumentWithAlias)
    {
        auto cmd = ImagePushCommand(L"image");

        bool found = false;
        for (const auto& arg : cmd.GetArguments())
        {
            if (arg.Type() == argument::ArgType::AllTags)
            {
                found = true;
                VERIFY_ARE_EQUAL(argument::Kind::Flag, arg.Kind());
                VERIFY_IS_FALSE(arg.Required());
                VERIFY_ARE_EQUAL(std::wstring(L"all-tags"), std::wstring(arg.Name()));
                VERIFY_ARE_EQUAL(std::wstring(L"a"), std::wstring(arg.Alias()));
                break;
            }
        }

        VERIFY_IS_TRUE(found, L"image push does not register --all-tags");
    }

    // Every command in the inspect family exposes docker's `-f` alias for --format
    // (docker/cli cli/command/system/inspect.go: flags.StringVarP(&opts.format, "format", "f", ...)).
    TEST_METHOD(InspectCommands_FormatArgumentHasDockerAlias)
    {
        const auto VerifyFormatAlias = [](const Command& command) {
            const auto args = command.GetArguments();
            const auto found = std::ranges::find_if(args, [](const auto& arg) { return arg.Type() == ArgType::InspectFormat; });

            VERIFY_IS_TRUE(found != args.end(), std::format(L"Command '{}' does not register --format", command.FullName()).c_str());
            VERIFY_ARE_EQUAL(std::wstring(L"format"), found->Name());
            VERIFY_ARE_EQUAL(std::wstring(L"f"), found->Alias());
        };

        VerifyFormatAlias(InspectCommand(L""));
        VerifyFormatAlias(ContainerInspectCommand(L"container"));
        VerifyFormatAlias(ImageInspectCommand(L"image"));
        VerifyFormatAlias(NetworkInspectCommand(L"network"));
        VerifyFormatAlias(VolumeInspectCommand(L"volume"));
    }

    // Walk every command in the root tree and verify declaration, source, name, and alias invariants.
    TEST_METHOD(AllCommands_NoArgumentDeclarationCollisions)
    {
        // Build a lookup table from ArgType -> enum name string using the same X-macro.
        static constexpr const wchar_t* c_argTypeNames[] = {
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, ConvertedType, Desc) L## #EnumName,
            WSLC_ARGUMENTS(WSLC_ARG_ENUM)
#undef WSLC_ARG_ENUM
        };

        const auto ArgTypeName = [](argument::ArgType type) -> std::wstring_view {
            const auto index = static_cast<size_t>(type);
            const auto max = static_cast<size_t>(argument::ArgType::Max);
            if (index < max)
            {
                return c_argTypeNames[index];
            }

            return L"<unknown>";
        };

        // Starting with the Root command, verify no argument collisions.
        RootCommand root;
        std::vector<std::reference_wrapper<const Command>> commands;
        commands.emplace_back(std::cref(root));
        std::unordered_set<size_t> commandLineTypes;
        std::unordered_set<size_t> environmentTypes;

        while (!commands.empty())
        {
            const auto& current = commands.back().get();
            commands.pop_back();

            const std::wstring commandFullName(current.FullName());
            std::unordered_set<size_t> seenTypes;
            std::unordered_map<std::wstring, argument::ArgType> seenNames;
            std::unordered_map<std::wstring, argument::ArgType> seenAliases;

            auto declaredArguments = current.GetArguments();
            const auto globalArguments = current.GetGlobalArguments();
            declaredArguments.insert(declaredArguments.end(), globalArguments.begin(), globalArguments.end());
            declaredArguments.emplace_back(Argument::Create(ArgType::Help));
            auto commandLineArguments = current.GetScopedArguments(Scope::Command, Flags::None);
            const auto globalCommandLineArguments = current.GetScopedArguments(Scope::Global, Flags::None);
            commandLineArguments.insert(commandLineArguments.end(), globalCommandLineArguments.begin(), globalCommandLineArguments.end());

            const auto VerifyUniqueType = [&](const Argument& argument) {
                if (!seenTypes.emplace(static_cast<size_t>(argument.Type())).second)
                {
                    VERIFY_FAIL(
                        std::format(
                            L"Command '{}' registers ArgType '{}' more than once across command-line and environment arguments",
                            commandFullName,
                            ArgTypeName(argument.Type()))
                            .c_str());
                }
            };

            const auto VerifyEnvironmentBinding = [&](const Argument& argument) {
                const auto binding = std::ranges::find(c_envBindings, argument.Type(), &EnvBinding::Type);
                VERIFY_IS_TRUE(
                    binding != std::end(c_envBindings),
                    std::format(
                        L"Command '{}' registers environment ArgType '{}' without a binding",
                        commandFullName,
                        ArgTypeName(argument.Type()))
                        .c_str());
            };

            for (const auto& arg : globalArguments)
            {
                VERIFY_IS_TRUE(
                    arg.IsOption(),
                    std::format(L"Command '{}' configures non-option '{}' as global", commandFullName, ArgTypeName(arg.Type())).c_str());
                VERIFY_IS_FALSE(
                    arg.Required(),
                    std::format(L"Command '{}' configures required option '{}' as global", commandFullName, ArgTypeName(arg.Type()))
                        .c_str());
            }

            for (const auto& arg : declaredArguments)
            {
                VerifyUniqueType(arg);

                if (arg.HasAnyFlag(Flags::EnvironmentOnly))
                {
                    VerifyEnvironmentBinding(arg);
                    VERIFY_IS_TRUE(
                        arg.IsOption(),
                        std::format(L"Command '{}' configures non-option '{}' as environment-only", commandFullName, ArgTypeName(arg.Type()))
                            .c_str());
                    environmentTypes.emplace(static_cast<size_t>(arg.Type()));
                }
                else
                {
                    commandLineTypes.emplace(static_cast<size_t>(arg.Type()));
                }
            }

            for (const auto& arg : commandLineArguments)
            {
                // Check name collision between distinct ArgTypes.
                const auto& name = arg.Name();
                if (name.size() < 2)
                {
                    VERIFY_FAIL(
                        std::format(L"Command '{}' uses invalid long-form name '--{}' for ArgType '{}'", commandFullName, name, ArgTypeName(arg.Type()))
                            .c_str());
                }

                auto [nameIt, nameInserted] = seenNames.emplace(name, arg.Type());
                if (!nameInserted)
                {
                    VERIFY_FAIL(std::format(
                                    L"Command '{}' has duplicate name '--{}' (ArgType '{}' conflicts with ArgType '{}')",
                                    commandFullName,
                                    name,
                                    ArgTypeName(arg.Type()),
                                    ArgTypeName(nameIt->second))
                                    .c_str());
                }

                // Check alias collision between distinct ArgTypes; skip empty aliases (NO_ALIAS).
                const auto& alias = arg.Alias();
                if (!alias.empty())
                {
                    auto [aliasIt, aliasInserted] = seenAliases.emplace(alias, arg.Type());
                    if (!aliasInserted)
                    {
                        VERIFY_FAIL(std::format(
                                        L"Command '{}' has duplicate alias '-{}' (ArgType '{}' conflicts with ArgType '{}')",
                                        commandFullName,
                                        alias,
                                        ArgTypeName(arg.Type()),
                                        ArgTypeName(aliasIt->second))
                                        .c_str());
                    }
                }
            }

            // Add any subcommands of this command for validation.
            for (const auto& subcommand : current.GetCommands())
            {
                commands.emplace_back(std::cref(*subcommand));
            }
        }

        for (const auto type : environmentTypes)
        {
            VERIFY_IS_FALSE(
                commandLineTypes.contains(type),
                std::format(L"Environment-only ArgType '{}' is also registered as a command-line argument", ArgTypeName(static_cast<ArgType>(type)))
                    .c_str());
        }
    }

    TEST_METHOD(AllCommands_NoInheritedGlobalArgumentCollisions)
    {
        struct PendingCommand
        {
            std::reference_wrapper<const Command> Command;
            std::vector<Argument> InheritedCliGlobals;
            std::vector<Argument> InheritedGlobals;
        };

        RootCommand root;
        std::vector<PendingCommand> pending;
        pending.emplace_back(PendingCommand{.Command = std::cref(root)});

        while (!pending.empty())
        {
            auto current = std::move(pending.back());
            pending.pop_back();
            const auto& command = current.Command.get();

            const auto cliGlobals = command.GetScopedArguments(Scope::Global, Flags::None);
            const auto globalArguments = command.GetScopedArguments(Scope::Global);

            for (const auto& argument : globalArguments)
            {
                const auto duplicateType = std::ranges::find(current.InheritedGlobals, argument.Type(), &Argument::Type);
                VERIFY_IS_TRUE(
                    duplicateType == current.InheritedGlobals.end(),
                    std::format(
                        L"Command '{}' reuses inherited global ArgType '{}'", command.FullName(), static_cast<size_t>(argument.Type()))
                        .c_str());
            }

            for (const auto& argument : cliGlobals)
            {
                const auto duplicateName = std::ranges::find_if(current.InheritedCliGlobals, [&](const auto& inherited) {
                    return wsl::shared::string::IsEqual(argument.Name(), inherited.Name());
                });
                VERIFY_IS_TRUE(
                    duplicateName == current.InheritedCliGlobals.end(),
                    std::format(L"Command '{}' reuses inherited global option name '--{}'", command.FullName(), argument.Name()).c_str());

                if (!argument.Alias().empty())
                {
                    const auto duplicateAlias = std::ranges::find_if(current.InheritedCliGlobals, [&](const auto& inherited) {
                        return !inherited.Alias().empty() && wsl::shared::string::IsEqual(argument.Alias(), inherited.Alias());
                    });
                    VERIFY_IS_TRUE(
                        duplicateAlias == current.InheritedCliGlobals.end(),
                        std::format(L"Command '{}' reuses inherited global option alias '-{}'", command.FullName(), argument.Alias())
                            .c_str());
                }
            }

            current.InheritedCliGlobals.insert(current.InheritedCliGlobals.end(), cliGlobals.begin(), cliGlobals.end());
            current.InheritedGlobals.insert(current.InheritedGlobals.end(), globalArguments.begin(), globalArguments.end());

            for (const auto& argument : command.GetScopedArguments(Scope::Command))
            {
                const auto duplicateType = std::ranges::find(current.InheritedGlobals, argument.Type(), &Argument::Type);
                VERIFY_IS_TRUE(
                    duplicateType == current.InheritedGlobals.end(),
                    std::format(
                        L"Command '{}' reuses inherited global ArgType '{}' as a command argument",
                        command.FullName(),
                        static_cast<size_t>(argument.Type()))
                        .c_str());
            }

            for (const auto& subcommand : command.GetCommands())
            {
                pending.emplace_back(PendingCommand{
                    .Command = std::cref(*subcommand),
                    .InheritedCliGlobals = current.InheritedCliGlobals,
                    .InheritedGlobals = current.InheritedGlobals,
                });
            }
        }
    }
};

} // namespace WSLCCLICommandUnitTests
