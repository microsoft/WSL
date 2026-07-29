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
#include "ArgumentValidation.h"
#include "ImageService.h"
#include "Exceptions.h"
#include <wslc.h>

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIArgumentUnitTests {
class WSLCCLIArgumentUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIArgumentUnitTests)

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
                args.Add(argType, std::wstring(L"test"));
                break;
            case Kind::Forward:
                args.Add(argType, std::vector<std::wstring>{L"forward1", L"forward2"});
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

    // Test: Verify Argument::Create() successfully creates arguments for all ArgType enum values
    TEST_METHOD(ArgumentValidation_ValueValidation)
    {
        // Verify integer conversion for supported types.
        auto longlong = validation::GetIntegerFromString<LONGLONG>(L"1234567890123");
        VERIFY_ARE_EQUAL(longlong, 1234567890123LL);
        VERIFY_THROWS(validation::GetIntegerFromString<LONGLONG>(L"abc"), ArgumentException);                      // Not a number
        VERIFY_THROWS(validation::GetIntegerFromString<LONGLONG>(L"-92233720369999854775808"), ArgumentException); // Out of range
        VERIFY_NO_THROW(validation::ValidateIntegerFromString<LONGLONG>({L"1234", L"-1234567890123"}, L"testArg"));
        VERIFY_THROWS(validation::ValidateIntegerFromString<LONGLONG>({L"1234", L"-92233720369999854775808"}, L"testArg"), ArgumentException);

        // Verify --tail validation rejects 0 (mirrors ArgType::Tail validation)
        VERIFY_THROWS(validation::ValidateIntegerFromString<ULONGLONG>({L"0"}, L"tail", [](auto value) { return value != 0; }), ArgumentException);
        VERIFY_NO_THROW(validation::ValidateIntegerFromString<ULONGLONG>({L"10"}, L"tail", [](auto value) { return value != 0; }));
        VERIFY_NO_THROW(validation::ValidateIntegerFromString<ULONGLONG>({L"1"}, L"tail", [](auto value) { return value != 0; }));

        // Verify WSLCSignal conversion
        auto validSignal = validation::GetWSLCSignalFromString(L"SIGTERM");
        VERIFY_ARE_EQUAL(validSignal, WSLCSignalSIGTERM);
        validSignal = validation::GetWSLCSignalFromString(L"TERM"); // No prefix
        VERIFY_ARE_EQUAL(validSignal, WSLCSignalSIGTERM);
        validSignal = validation::GetWSLCSignalFromString(L"sIgTerm"); // Case-insensitive
        VERIFY_ARE_EQUAL(validSignal, WSLCSignalSIGTERM);
        validSignal = validation::GetWSLCSignalFromString(L"term"); // Case-insensitive no prefix
        VERIFY_ARE_EQUAL(validSignal, WSLCSignalSIGTERM);
        VERIFY_THROWS(validation::GetWSLCSignalFromString(L"INVALID_SIGNAL"), ArgumentException);
        validSignal = validation::GetWSLCSignalFromString(L"15"); // SIGTERM is 15
        VERIFY_ARE_EQUAL(validSignal, WSLCSignalSIGTERM);
        VERIFY_THROWS(validation::GetWSLCSignalFromString(L"999"), ArgumentException); // Out of range
        VERIFY_NO_THROW(validation::ValidateWSLCSignalFromString({L"HUP", L"9", L"SIGKILL", L"stop"}, L"signalArg"));
        VERIFY_THROWS(validation::ValidateWSLCSignalFromString({L"SIGHUP", L"999"}, L"signalArg"), ArgumentException); // 999 is out of range

        // Verify format type
        auto format = validation::GetFormatTypeFromString(L"json");
        VERIFY_ARE_EQUAL(format, FormatType::Json);
        format = validation::GetFormatTypeFromString(L"table");
        VERIFY_ARE_EQUAL(format, FormatType::Table);
        VERIFY_THROWS(validation::GetFormatTypeFromString(L"xml"), ArgumentException);
        VERIFY_NO_THROW(validation::ValidateFormatTypeFromString({L"json", L"table"}, L"formatArg"));
        VERIFY_THROWS(validation::ValidateFormatTypeFromString({L"JSON", L"TABLE", L"csv"}, L"formatArg"), ArgumentException);

        // Verify GPU device argument
        VERIFY_NO_THROW(validation::ValidateGpus({L"all"}, L"gpusArg"));
        VERIFY_THROWS(validation::ValidateGpus({L"none"}, L"gpusArg"), ArgumentException);
        VERIFY_THROWS(validation::ValidateGpus({L"0"}, L"gpusArg"), ArgumentException);
        VERIFY_THROWS(validation::ValidateGpus({L"gpu0"}, L"gpusArg"), ArgumentException);
        VERIFY_THROWS(validation::ValidateGpus({L""}, L"gpusArg"), ArgumentException);
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
        argsContainer.Add<ArgType::ForwardArgs>(std::vector<std::wstring>{L"test1", L"test2"});
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
        VERIFY_ARE_EQUAL(allArgTypes.size(), 4);
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Help) != allArgTypes.end());
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::ContainerId) != allArgTypes.end());
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::Publish) != allArgTypes.end());
        VERIFY_IS_TRUE(std::find(allArgTypes.begin(), allArgTypes.end(), ArgType::ForwardArgs) != allArgTypes.end());

        // Verify count
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Help), 1);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::ContainerId), 1);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::ForwardArgs), 1);
        VERIFY_ARE_EQUAL(argsContainer.GetCount(), 6); // 1 Help + 1 ContainerId + 3 Publish + 1 ForwardArgs
        argsContainer.Remove(ArgType::Help);
        argsContainer.Remove(ArgType::ContainerId);
        argsContainer.Remove(ArgType::Publish);
        argsContainer.Remove(ArgType::ForwardArgs);
        VERIFY_ARE_EQUAL(argsContainer.GetCount(), 0);
    }

    // Test: Verify the validated-value cache stores and returns converted results so that a
    // conversion performed during validation is reused during execution. Access is by a compile-time
    // ArgType, so the value type is fixed by the argument's ConvertedType and cannot be mismatched.
    TEST_METHOD(ValidatedCache_StoresAndRetrievesConvertedValues)
    {
        ArgMap args;

        // Populate raw arguments so the validated-cache invariant (raw count == validated count,
        // enforced by a debug assert in the cache readers) holds when values are read below.
        args.Add(ArgType::StopTimeout, std::wstring(L"30"));
        args.Add(ArgType::Filter, std::wstring(L"status=running"));
        args.Add(ArgType::Filter, std::wstring(L"label=env=prod"));

        // Nothing cached yet.
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::StopTimeout));

        // A conversion that produces a non-string type (string -> int). The value type is fixed by
        // ArgType::StopTimeout's ConvertedType (int), so no type is supplied by the caller.
        args.AddValidated<ArgType::StopTimeout>(30);
        VERIFY_IS_TRUE(args.ContainsValidated(ArgType::StopTimeout));
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::StopTimeout), static_cast<size_t>(1));
        VERIFY_ARE_EQUAL(args.GetValue<ArgType::StopTimeout>(), 30);

        // Multiple cached values for one argument preserve insertion order.
        args.AddValidated<ArgType::Filter>(std::pair<std::string, std::string>{"status", "running"});
        args.AddValidated<ArgType::Filter>(std::pair<std::string, std::string>{"label", "env=prod"});
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Filter), static_cast<size_t>(2));
        auto filters = args.GetAllValues<ArgType::Filter>();
        VERIFY_ARE_EQUAL(filters.size(), static_cast<size_t>(2));
        VERIFY_ARE_EQUAL(filters[0].first, std::string("status"));
        VERIFY_ARE_EQUAL(filters[0].second, std::string("running"));
        VERIFY_ARE_EQUAL(filters[1].first, std::string("label"));
        VERIFY_ARE_EQUAL(filters[1].second, std::string("env=prod"));

        // GetAllValidated returns empty when nothing is cached for the argument.
        auto empty = args.GetAllValues<ArgType::Signal>();
        VERIFY_IS_TRUE(empty.empty());

        // Reading an argument that was never validated throws E_NOT_SET.
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::Memory));
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Memory), static_cast<size_t>(0));
        VERIFY_THROWS_SPECIFIC(args.GetValue<ArgType::Memory>(), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_NOT_SET;
        });
    }

    // Helper: run the real validation path for a single-value argument and return the cached,
    // converted result. The result type is fixed by the argument's ConvertedType. Also asserts
    // that validation populated the cache for that argument.
    template <ArgType E>
    static auto ValidateAndGetCached(const std::wstring& raw)
    {
        ArgMap args;
        args.Add(E, std::wstring(raw));
        Argument::Create(E).Validate(args);
        VERIFY_IS_TRUE(args.ContainsValidated(E));
        return args.GetValue<E>();
    }

    // Helper: run the real validation path for an argument that appears multiple times (ArgMap is
    // a multimap) and return every cached, converted value in insertion order.
    template <ArgType E>
    static auto ValidateAndGetAllCached(const std::vector<std::wstring>& raws)
    {
        ArgMap args;
        for (const auto& raw : raws)
        {
            args.Add(E, std::wstring(raw));
        }

        Argument::Create(E).Validate(args);

        // The cache must hold exactly one converted value per raw value in the map.
        VERIFY_ARE_EQUAL(args.CountValidated(E), args.Count(E));
        VERIFY_ARE_EQUAL(args.CountValidated(E), raws.size());
        return args.GetAllValues<E>();
    }

    // Test: Every ArgType whose validation converts its raw string into a typed value must cache
    // that value on the ArgMap during Argument::Validate, so execution reads it back without
    // re-converting. This drives the real validation + caching path for each converted ArgType.
    TEST_METHOD(ArgumentValidate_ConvertsAndCachesEveryConvertedArgType)
    {
        // string -> FormatType
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Format>(L"json"), FormatType::Json);

        // string -> WSLCSignal (Signal and StopSignal share the converter)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Signal>(L"SIGTERM"), WSLCSignalSIGTERM);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::StopSignal>(L"SIGKILL"), WSLCSignalSIGKILL);

        // string -> int
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::StopTimeout>(L"30"), 30);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthRetries>(L"3"), 3);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Last>(L"5"), 5);

        // string -> LONG
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Time>(L"5"), 5L);

        // string -> ULONGLONG (Tail is a raw integer; Since/Until go through the timestamp parser)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Tail>(L"10"), 10ULL);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Since>(L"100"), validation::GetTimestampFromString(L"100"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Until>(L"200"), validation::GetTimestampFromString(L"200"));

        // string -> int64_t (memory sizes). The cached value matches the converter's result.
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Memory>(L"512M"), validation::GetMemorySizeFromString(L"512M"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::ShmSize>(L"64M"), validation::GetMemorySizeFromString(L"64M"));

        // string -> int64_t (durations, in nanoseconds)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthInterval>(L"30s"), validation::GetDurationNanosFromString(L"30s"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthTimeout>(L"30s"), validation::GetDurationNanosFromString(L"30s"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthStartPeriod>(L"30s"), validation::GetDurationNanosFromString(L"30s"));

        // string -> int64_t (nano CPUs)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Cpus>(L"1.5"), validation::GetNanoCpusFromString(L"1.5"));

        // string -> tuple<name, soft, hard> (ulimit)
        auto ulimit = ValidateAndGetCached<ArgType::Ulimit>(L"nofile=1024:2048");
        VERIFY_ARE_EQUAL(std::get<0>(ulimit), std::string("nofile"));
        VERIFY_ARE_EQUAL(std::get<1>(ulimit), 1024LL);
        VERIFY_ARE_EQUAL(std::get<2>(ulimit), 2048LL);

        // string -> pair<key, value> (filter). A single Validate call caches every raw value in order.
        {
            ArgMap args;
            args.Add(ArgType::Filter, std::wstring(L"status=running"));
            args.Add(ArgType::Filter, std::wstring(L"label=env=prod")); // split on first '='
            Argument::Create(ArgType::Filter).Validate(args);
            auto filters = args.GetAllValues<ArgType::Filter>();
            VERIFY_ARE_EQUAL(filters.size(), static_cast<size_t>(2));
            VERIFY_ARE_EQUAL(filters[0].first, std::string("status"));
            VERIFY_ARE_EQUAL(filters[0].second, std::string("running"));
            VERIFY_ARE_EQUAL(filters[1].first, std::string("label"));
            VERIFY_ARE_EQUAL(filters[1].second, std::string("env=prod"));
        }

        // string -> InspectType (inspect object type)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Type>(L"container"), InspectType::Container);

        // string -> pair<key, value> (label and driver option share the key=value shape)
        {
            auto label = ValidateAndGetCached<ArgType::Label>(L"env=prod");
            VERIFY_ARE_EQUAL(label.first, std::string("env"));
            VERIFY_ARE_EQUAL(label.second, std::string("prod"));

            auto option = ValidateAndGetCached<ArgType::Options>(L"com.docker.network.bridge.name=br0");
            VERIFY_ARE_EQUAL(option.first, std::string("com.docker.network.bridge.name"));
            VERIFY_ARE_EQUAL(option.second, std::string("br0"));
        }

        // string -> BuildSecret (docker-style --secret spec resolved to an id and value bytes)
        {
            ScopedEnvVariable env(L"WSLC_UT_CONV_SECRET", L"conv-value");
            auto secret = ValidateAndGetCached<ArgType::Secret>(L"id=convtest,env=WSLC_UT_CONV_SECRET");
            VERIFY_ARE_EQUAL(secret.Id, std::wstring(L"convtest"));
            const std::string expected = "conv-value";
            VERIFY_IS_TRUE(std::vector<BYTE>(expected.begin(), expected.end()) == secret.Value);
        }
    }

    // Test: Because ArgMap is a multimap and any command may allow an argument to repeat, a single
    // Argument::Validate call must convert and cache every occurrence, in order. Covers the
    // different converter result shapes (integer, enum, tuple, pair). The helper also asserts the
    // cached count matches the number of raw values in the map.
    TEST_METHOD(ArgumentValidate_CachesEveryValueForRepeatedArg)
    {
        // Integer converter, multiple values -> all cached in order.
        auto retries = ValidateAndGetAllCached<ArgType::HealthRetries>({L"1", L"2", L"3"});
        VERIFY_ARE_EQUAL(retries.size(), static_cast<size_t>(3));
        VERIFY_ARE_EQUAL(retries[0], 1);
        VERIFY_ARE_EQUAL(retries[1], 2);
        VERIFY_ARE_EQUAL(retries[2], 3);

        // int64_t converter (memory sizes), multiple values -> all cached in order.
        auto memories = ValidateAndGetAllCached<ArgType::Memory>({L"128M", L"256M"});
        VERIFY_ARE_EQUAL(memories.size(), static_cast<size_t>(2));
        VERIFY_ARE_EQUAL(memories[0], validation::GetMemorySizeFromString(L"128M"));
        VERIFY_ARE_EQUAL(memories[1], validation::GetMemorySizeFromString(L"256M"));

        // Enum converter, multiple values -> all cached in order.
        auto signals = ValidateAndGetAllCached<ArgType::Signal>({L"SIGTERM", L"SIGKILL", L"SIGHUP"});
        VERIFY_ARE_EQUAL(signals.size(), static_cast<size_t>(3));
        VERIFY_ARE_EQUAL(signals[0], WSLCSignalSIGTERM);
        VERIFY_ARE_EQUAL(signals[1], WSLCSignalSIGKILL);
        VERIFY_ARE_EQUAL(signals[2], WSLCSignalSIGHUP);

        // Tuple converter (ulimit), multiple values -> all cached in order.
        auto ulimits = ValidateAndGetAllCached<ArgType::Ulimit>({L"nofile=1024:2048", L"nproc=512:1024"});
        VERIFY_ARE_EQUAL(ulimits.size(), static_cast<size_t>(2));
        VERIFY_ARE_EQUAL(std::get<0>(ulimits[0]), std::string("nofile"));
        VERIFY_ARE_EQUAL(std::get<1>(ulimits[0]), 1024LL);
        VERIFY_ARE_EQUAL(std::get<2>(ulimits[0]), 2048LL);
        VERIFY_ARE_EQUAL(std::get<0>(ulimits[1]), std::string("nproc"));
        VERIFY_ARE_EQUAL(std::get<1>(ulimits[1]), 512LL);
        VERIFY_ARE_EQUAL(std::get<2>(ulimits[1]), 1024LL);

        // Pair converter (filter), multiple values -> all cached in order.
        auto filters = ValidateAndGetAllCached<ArgType::Filter>({L"status=running", L"name=web", L"label=env=prod"});
        VERIFY_ARE_EQUAL(filters.size(), static_cast<size_t>(3));
        VERIFY_ARE_EQUAL(filters[0].first, std::string("status"));
        VERIFY_ARE_EQUAL(filters[0].second, std::string("running"));
        VERIFY_ARE_EQUAL(filters[1].first, std::string("name"));
        VERIFY_ARE_EQUAL(filters[1].second, std::string("web"));
        VERIFY_ARE_EQUAL(filters[2].first, std::string("label"));
        VERIFY_ARE_EQUAL(filters[2].second, std::string("env=prod"));

        // BuildSecret converter (secret specs), multiple values -> all cached in order.
        {
            ScopedEnvVariable envA(L"WSLC_UT_CONV_SECRET_A", L"value-a");
            ScopedEnvVariable envB(L"WSLC_UT_CONV_SECRET_B", L"value-b");
            auto secrets = ValidateAndGetAllCached<ArgType::Secret>(
                {L"id=seca,env=WSLC_UT_CONV_SECRET_A", L"id=secb,env=WSLC_UT_CONV_SECRET_B"});
            VERIFY_ARE_EQUAL(secrets.size(), static_cast<size_t>(2));
            VERIFY_ARE_EQUAL(secrets[0].Id, std::wstring(L"seca"));
            VERIFY_ARE_EQUAL(secrets[1].Id, std::wstring(L"secb"));
            const std::string expectedA = "value-a";
            const std::string expectedB = "value-b";
            VERIFY_IS_TRUE(std::vector<BYTE>(expectedA.begin(), expectedA.end()) == secrets[0].Value);
            VERIFY_IS_TRUE(std::vector<BYTE>(expectedB.begin(), expectedB.end()) == secrets[1].Value);
        }
    }

    // Test: Every validate-only ArgType (checked during validation but not converted into a
    // distinct typed value that execution consumes) must NOT populate the cache. Execution reads
    // the raw value for these instead.
    TEST_METHOD(ArgumentValidate_ValidateOnlyArgsAreNotCached)
    {
        struct Case
        {
            ArgType Type;
            std::wstring Value;
        };

        const std::vector<Case> cases = {
            {ArgType::Gpus, L"all"},
            {ArgType::Volume, LR"(C:\hostPath:/containerPath)"},
            {ArgType::WorkDir, L"/app"},
            {ArgType::Network, L"bridge"},
            {ArgType::NetworkAlias, L"myalias"},
        };

        for (const auto& c : cases)
        {
            ArgMap args;
            args.Add(c.Type, std::wstring(c.Value));
            Argument::Create(c.Type).Validate(args);
            VERIFY_IS_FALSE(args.ContainsValidated(c.Type));
        }

        // NoHealthcheck is a flag whose validation only rejects conflicting health options. With
        // no conflicts present it passes and caches nothing.
        ArgMap noHealthcheck;
        noHealthcheck.Add(ArgType::NoHealthcheck, true);
        Argument::Create(ArgType::NoHealthcheck).Validate(noHealthcheck);
        VERIFY_IS_FALSE(noHealthcheck.ContainsValidated(ArgType::NoHealthcheck));
    }

    // Test: When conversion fails during validation, Validate throws and nothing is cached.
    TEST_METHOD(ArgumentValidate_InvalidValueThrowsAndCachesNothing)
    {
        ArgMap args;
        args.Add(ArgType::Format, std::wstring(L"xml"));
        VERIFY_THROWS(Argument::Create(ArgType::Format).Validate(args), ArgumentException);
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::Format));
    }

    // Timestamp parsing unit tests (exercises TryParseRfc3339 and integer path via GetTimestampFromString)

    TEST_METHOD(ValidateTimestamp_ValidUnixEpochSeconds)
    {
        // Integer timestamps should parse directly
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"0"), 0ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1700000000"), 1700000000ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1"), 1ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"9999999999"), 9999999999ULL);
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_UTC)
    {
        // Basic UTC timestamps with Z suffix
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00Z"), 1705314600ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1970-01-01T00:00:00Z"), 0ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00z"), 1705314600ULL); // lowercase z
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_WithOffset)
    {
        // Timestamps with timezone offsets (+HH:MM / -HH:MM)
        // 2024-01-15T10:30:00+05:30 = 2024-01-15T05:00:00Z = 1705294800
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00+05:30"), 1705294800ULL);
        // 2024-01-15T10:30:00-05:00 = 2024-01-15T15:30:00Z = 1705332600
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00-05:00"), 1705332600ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00+00:00"), 1705314600ULL);
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_FractionalSeconds)
    {
        // Fractional seconds should be consumed (truncated to seconds)
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.123Z"), 1705314600ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.123456789Z"), 1705314600ULL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.1+05:30"), 1705294800ULL);
    }

    TEST_METHOD(ValidateTimestamp_InvalidRfc3339_Rejected)
    {
        // Invalid month
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-13-15T10:30:00Z"), ArgumentException);
        // Invalid hour
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T25:30:00Z"), ArgumentException);
        // Invalid day (Feb 31)
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-02-31T10:30:00Z"), ArgumentException);
        // Missing timezone
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00"), ArgumentException);
        // Date only (no time)
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15"), ArgumentException);
        // Trailing characters
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00Zextra"), ArgumentException);
        // +HHMM without colon (not supported by %Ez)
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00+0530"), ArgumentException);
        // Dot with no fractional digits
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00.Z"), ArgumentException);
        // Random text
        VERIFY_THROWS(validation::GetTimestampFromString(L"abc"), ArgumentException);
        VERIFY_THROWS(validation::GetTimestampFromString(L"not-a-timestamp"), ArgumentException);
        // Negative epoch (pre-1970)
        VERIFY_THROWS(validation::GetTimestampFromString(L"1960-01-15T10:30:00Z"), ArgumentException);
    }
};
} // namespace WSLCCLIArgumentUnitTests
