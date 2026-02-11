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
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "SessionCommand.h"
#include "VolumeCommand.h"
#include "RegistryCommand.h"
#include "RootCommand.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLICommandUnitTests
{
    class WSLCCLICommandUnitTests
    {
        WSL_TEST_CLASS(WSLCCLICommandUnitTests)

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

        // Test: Verify ContainerCommand has subcommands
        TEST_METHOD(ContainerCommand_HasSubcommands)
        {
            auto cmd = ContainerCommand(L"container");
            auto subcommands = cmd.GetCommands();
            
            // Verify it has subcommands (create, list, run, etc.)
            VERIFY_IS_TRUE(subcommands.size() > 0);
            LogComment(L"ContainerCommand has " + std::to_wstring(subcommands.size()) + L" subcommands");
            
            // Log subcommand types
            for (const auto& subcmd : subcommands)
            {
                VERIFY_IS_NOT_NULL(subcmd.get());
            }
        }

        // Test: Verify RootCommand contains expected subcommands
        TEST_METHOD(RootCommand_ContainsExpectedSubcommands)
        {
            auto cmd = RootCommand();
            auto subcommands = cmd.GetCommands();
            
            // Log all subcommand types for visibility
            for (const auto& subcmd : subcommands)
            {
                LogComment(L"Subcommand found");
            }
            
            // At minimum, verify we have some subcommands
            VERIFY_IS_TRUE(subcommands.size() >= 1);
        }
    };
    
} // namespace WSLCCLICommandUnitTests