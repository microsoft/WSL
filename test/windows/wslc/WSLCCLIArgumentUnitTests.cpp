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

        // Test: Verify Argument::Create() successfully creates arguments for all ArgType enum values
        TEST_METHOD(ArgumentCreate_AllArguments)
        {
            // Iterate through all ArgType enum values except Max
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

                // Create Args collection and add a valid type and emplace into the map successfully
                // using the runtime function Add instead of the template function Add.
                ArgMap args;
                VERIFY_IS_FALSE(args.Contains(argType));
                auto valueType = args.GetValueType(argType);
                switch (valueType)
                {
                    case ValueType::String:
                        args.Add(argType, std::wstring(L"test"));
                        break;
                    case ValueType::StringSet:
                        args.Add(argType, std::vector<std::wstring>{ L"test1", L"test2" });
                        break;
                    case ValueType::Bool:
                        args.Add(argType, true);
                        break;
                    default:
                        VERIFY_FAIL(L"Unhandled ValueType in test");
                }

                VERIFY_IS_TRUE(args.Contains(argType));

                // Retrieval is type specific, so we will verify that with a differnet test.
            }
        }

        // Test: Verify EnumVariantMap
        TEST_METHOD(EnumVariantMap_AllDataTypes)
        {
            // ArgMap is an EnumVariantMap
            ArgMap argsContainer;

            // Verify basic add
            argsContainer.Add<ArgType::Help>(true);
            VERIFY_IS_TRUE(argsContainer.Contains(ArgType::Help));
            argsContainer.Add<ArgType::ContainerId>(std::wstring(L"test"));
            VERIFY_IS_TRUE(argsContainer.Contains(ArgType::ContainerId));
            argsContainer.Add<ArgType::ForwardArgs>(std::vector<std::wstring>{ L"test1", L"test2" });
            VERIFY_IS_TRUE(argsContainer.Contains(ArgType::ForwardArgs));

            // Verify basic retrieval
            auto retrievedBool = argsContainer.Get<ArgType::Help>();
            VERIFY_ARE_EQUAL(retrievedBool, true);
            auto retrievedString = argsContainer.Get<ArgType::ContainerId>();
            VERIFY_ARE_EQUAL(retrievedString, std::wstring(L"test"));
            auto retrievedStringSet = argsContainer.Get<ArgType::ForwardArgs>();
            VERIFY_ARE_EQUAL(retrievedStringSet[0], std::wstring(L"test1"));
            VERIFY_ARE_EQUAL(retrievedStringSet[1], std::wstring(L"test2"));

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
            argsContainer.RemoveOne(ArgType::Publish);
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 2);
            argsContainer.Remove(ArgType::Publish);
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 0);

            // Verify Set overrides all previous data.
            // String add works with a literal for template add (not runtime add)
            argsContainer.Add<ArgType::Publish>(L"test1");
            argsContainer.Add<ArgType::Publish>(L"test2");
            argsContainer.Add<ArgType::Publish>(L"test3");
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
            argsContainer.Set<ArgType::Publish>(L"test4");
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 1);
            publishArgs = argsContainer.GetAll<ArgType::Publish>();
            VERIFY_ARE_EQUAL(publishArgs.size(), 1);
            VERIFY_ARE_EQUAL(publishArgs[0], std::wstring(L"test4"));

            // Verify Keys
            auto allArgTypes = argsContainer.GetKeys();
            VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Help) != allArgTypes.end());
            VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::ContainerId) != allArgTypes.end());
            VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::ForwardArgs) != allArgTypes.end());
            VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Publish) != allArgTypes.end());

            // Verify count
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Help), 1);
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::ContainerId), 1);
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::ForwardArgs), 1);
            VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 1);
            VERIFY_ARE_EQUAL(argsContainer.GetCount(), 4);
            argsContainer.Remove(ArgType::Help);
            argsContainer.Remove(ArgType::ContainerId);
            argsContainer.Remove(ArgType::ForwardArgs);
            argsContainer.Remove(ArgType::Publish);
            VERIFY_ARE_EQUAL(argsContainer.GetCount(), 0);
        }
    };
} // namespace WSLCCLIArgumentUnitTests