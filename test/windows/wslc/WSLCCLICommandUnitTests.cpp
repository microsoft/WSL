/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLICommandUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI Command classes.

--*/

#include "precomp.h"
#include <unordered_map>
#include <unordered_set>
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Command.h"
#include "RootCommand.h"
#include "ContainerCommand.h"
#include "SessionCommand.h"
#include "VersionCommand.h"

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

        auto subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"RootCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Verify each subcommand is valid
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    // Test: Verify SessionCommand has subcommands
    TEST_METHOD(SessionCommand_HasSubcommands)
    {
        auto cmd = SessionCommand(L"session");
        auto subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"SessionCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Log subcommand types
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
        }
    }

    // Test: Verify SessionEnterCommand has the expected arguments
    TEST_METHOD(SessionEnterCommand_HasExpectedArguments)
    {
        auto cmd = SessionEnterCommand(L"session");
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
        auto cmd = SessionEnterCommand(L"session");

        VERIFY_IS_FALSE(cmd.ShortDescription().empty());
        VERIFY_IS_FALSE(cmd.LongDescription().empty());
    }

    // Test: Verify ContainerCommand has subcommands
    TEST_METHOD(ContainerCommand_HasSubcommands)
    {
        auto cmd = ContainerCommand(L"container");
        auto subcommands = cmd.GetCommands();

        // Verify it has subcommands
        VERIFY_IS_TRUE(subcommands.size() > 0);
        LogComment(L"ContainerCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");

        // Log subcommand types
        for (const auto& subcmd : subcommands)
        {
            VERIFY_IS_NOT_NULL(subcmd.get());
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

    // Test: Verify VersionCommand has no arguments (only the auto-added --help)
    TEST_METHOD(VersionCommand_HasNoArguments)
    {
        auto cmd = VersionCommand(L"wslc");
        VERIFY_ARE_EQUAL(0u, cmd.GetArguments().size());
        // Test out that auto added help command is the only one
        VERIFY_ARE_EQUAL(1u, cmd.GetAllArguments().size());
    }

    // Test: Verify RootCommand contains VersionCommand as a subcommand
    TEST_METHOD(RootCommand_ContainsVersionCommand)
    {
        auto root = RootCommand();
        auto subcommands = root.GetCommands();

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

    // Walk every command in the root tree and verify no argument collisions.
    TEST_METHOD(AllCommands_NoAmbiguousArgumentNamesOrAliases)
    {
        // Build a lookup table from ArgType -> enum name string using the same X-macro.
        static constexpr const wchar_t* c_argTypeNames[] = {
#define WSLC_ARG_ENUM(EnumName, Name, Alias, Kind, Desc) L## #EnumName,
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
        std::vector<std::unique_ptr<Command>> commands;
        commands.push_back(std::make_unique<RootCommand>());

        while (!commands.empty())
        {
            auto current = std::move(commands.back());
            commands.pop_back();
            VERIFY_IS_NOT_NULL(current.get());

            const std::wstring commandFullName(current->FullName());
            std::unordered_set<size_t> seenTypes;
            std::unordered_map<std::wstring, argument::ArgType> seenNames;
            std::unordered_map<std::wstring, argument::ArgType> seenAliases;

            for (const auto& arg : current->GetAllArguments())
            {
                // Check for duplicate ArgType registration.
                if (!seenTypes.emplace(static_cast<size_t>(arg.Type())).second)
                {
                    VERIFY_FAIL(std::format(L"Command '{}' registers ArgType '{}' more than once", commandFullName, ArgTypeName(arg.Type()))
                                    .c_str());
                }

                // Check name collision between distinct ArgTypes.
                const auto& name = arg.Name();
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
            for (auto& sub : current->GetCommands())
            {
                commands.push_back(std::move(sub));
            }
        }
    }
};

} // namespace WSLCCLICommandUnitTests
