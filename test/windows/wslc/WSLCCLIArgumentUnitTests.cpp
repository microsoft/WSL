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

namespace WSLCCLIArgumentUnitTests {
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

    // Test: Verify Argument::Create() successfully creates arguments for all ArgType enum values
    TEST_METHOD(ArgumentCreate_AllArguments)
    {
        // ArgMap is the container for processed args.
        ArgMap args;

        // Iterate through all ArgType enum values except Max
        auto allArgTypes = std::vector<ArgType>{};
        for (int i = 0; i < static_cast<int>(ArgType::Max); ++i)
        {
            ArgType argType = static_cast<ArgType>(i);

            // Create argument using Create
            Argument arg = Argument::Create(argType);

            // Verify the argument was created successfully by checking its type matches
            VERIFY_ARE_EQUAL(static_cast<int>(arg.Type()), i);

            // Verify the argument has basic properties set
            // (Name should not be empty for valid argument types)
            VERIFY_IS_FALSE(arg.Name().empty());
            LogComment(L"Verified Argument::Create() creates argument with name: " + arg.Name());

            // Add the argument to the ArgMap with a test value based on its type.
            VERIFY_IS_FALSE(args.Contains(argType));
            switch (arg.Kind())
            {
            case Kind::Value:
            case Kind::Positional:
            case Kind::Forward:
                args.Add(argType, std::wstring(L"test"));
                break;
            case Kind::Flag:
                args.Add(argType, true);
                break;
            default:
                VERIFY_FAIL(L"Unhandled ValueType in test");
            }

            allArgTypes.push_back(argType);
            VERIFY_IS_TRUE(args.Contains(argType));
        }

        // We do not have a runtime Get for argument values, so we will instead use the keys
        // in the argmap. The fact that the keys exist and can be used to retrieve values
        // verifies that Argument::Create() created arguments that are compatible with ArgMap.
        // Verify all created argument types are in the ArgMap keys
        auto argMapKeys = args.GetKeys();
        VERIFY_ARE_EQUAL(argMapKeys.size(), allArgTypes.size());
        for (const auto& argType : allArgTypes)
        {
            VERIFY_IS_TRUE(std::find(argMapKeys.begin(), argMapKeys.end(), argType) != argMapKeys.end());
        }
    }

    // Test: Verify EnumVariantMap behavior with ArgTypes.
    TEST_METHOD(EnumVariantMap_AllDataTypes)
    {
        // ArgMap is an EnumVariantMap
        ArgMap argsContainer;

        // Verify basic add
        argsContainer.Add<ArgType::Help>(true);
        VERIFY_IS_TRUE(argsContainer.Contains(ArgType::Help));
        argsContainer.Add<ArgType::ContainerId>(std::wstring(L"test"));
        VERIFY_IS_TRUE(argsContainer.Contains(ArgType::ContainerId));

        // Verify basic retrieval
        auto retrievedBool = argsContainer.Get<ArgType::Help>();
        VERIFY_ARE_EQUAL(retrievedBool, true);
        auto retrievedString = argsContainer.Get<ArgType::ContainerId>();
        VERIFY_ARE_EQUAL(retrievedString, std::wstring(L"test"));

        // Verify multimap functionality and Runtime Add
        argsContainer.Add(ArgType::Publish, std::wstring(L"test1"));
        argsContainer.Add(ArgType::Publish, std::wstring(L"test2"));
        argsContainer.Add(ArgType::Publish, std::wstring(L"test3"));
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
        auto publishArgs = argsContainer.GetAll<ArgType::Publish>();
        VERIFY_ARE_EQUAL(publishArgs.size(), 3);
        VERIFY_ARE_EQUAL(publishArgs[0], std::wstring(L"test1"));
        VERIFY_ARE_EQUAL(publishArgs[1], std::wstring(L"test2"));
        VERIFY_ARE_EQUAL(publishArgs[2], std::wstring(L"test3"));

        // Verify Remove
        argsContainer.Remove(ArgType::Publish);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 0);

        // Verify compile time add works like runtime add for multimap types.
        argsContainer.Add<ArgType::Publish>(L"test1");
        argsContainer.Add<ArgType::Publish>(L"test2");
        argsContainer.Add<ArgType::Publish>(L"test3");
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
        publishArgs = argsContainer.GetAll<ArgType::Publish>();
        VERIFY_ARE_EQUAL(publishArgs.size(), 3);
        VERIFY_ARE_EQUAL(publishArgs[0], std::wstring(L"test1"));
        VERIFY_ARE_EQUAL(publishArgs[1], std::wstring(L"test2"));
        VERIFY_ARE_EQUAL(publishArgs[2], std::wstring(L"test3"));

        // Verify Keys
        auto allArgTypes = argsContainer.GetKeys();
        VERIFY_ARE_EQUAL(allArgTypes.size(), 3);
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Help) != allArgTypes.end());
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::ContainerId) != allArgTypes.end());
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Publish) != allArgTypes.end());

        // Verify count
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Help), 1);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::ContainerId), 1);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
        VERIFY_ARE_EQUAL(argsContainer.GetCount(), 5); // 1 Help + 1 ContainerId + 3 Publish
        argsContainer.Remove(ArgType::Help);
        argsContainer.Remove(ArgType::ContainerId);
        argsContainer.Remove(ArgType::Publish);
        VERIFY_ARE_EQUAL(argsContainer.GetCount(), 0);
    }
};
} // namespace WSLCCLIArgumentUnitTests