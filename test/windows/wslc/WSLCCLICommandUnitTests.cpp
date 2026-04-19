/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLICommandUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI Command classes.

--*/

#include "precomp.h"
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

    // Test: Verify VersionCommand has the expected arguments (--format, plus the auto-added --help)
    TEST_METHOD(VersionCommand_HasFormatArgument)
    {
        auto cmd = VersionCommand(L"wslc");
        VERIFY_ARE_EQUAL(1u, cmd.GetArguments().size());
        // Test out that --format and auto-added --help are the only arguments
        VERIFY_ARE_EQUAL(2u, cmd.GetAllArguments().size());
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
};

} // namespace WSLCCLICommandUnitTests