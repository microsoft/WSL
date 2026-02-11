/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIArgumentUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Argument.h"
#include "ArgumentTypes.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIArgumentUnitTests
{
    class WSLCCLIArgumentUnitTests
    {
        WSL_TEST_CLASS(WSLCCLIArgumentUnitTests)

        TEST_CLASS_SETUP(TestClassSetup)
        {
            // Add any necessary setup for argument tests
            return true;
        }

        TEST_CLASS_CLEANUP(TestClassCleanup)
        {
            // Add any necessary cleanup for argument tests
            return true;
        }

        // Test: Verify Argument::ForType() successfully creates arguments for all ArgType enum values
        TEST_METHOD(ArgumentForType_CreatesAllArgumentTypes)
        {
            // Iterate through all ArgType enum values except Max
            for (int i = 0; i < static_cast<int>(ArgType::Max); ++i)
            {
                ArgType argType = static_cast<ArgType>(i);
        
                // Create argument using ForType
                Argument arg = Argument::ForType(argType);
        
                // Verify the argument was created successfully by checking its type matches
                VERIFY_ARE_EQUAL(static_cast<int>(arg.Type()), i);
        
                // Verify the argument has basic properties set
                // (Name should not be empty for valid argument types)
                VERIFY_IS_FALSE(arg.Name().empty());
                LogComment(L"Verified Argument::ForType() creates argument with name: " + arg.Name());
            }
        }
    };
} // namespace WSLCCLIArgumentUnitTests