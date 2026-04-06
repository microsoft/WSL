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
};

} // namespace WSLCCLICommandUnitTests