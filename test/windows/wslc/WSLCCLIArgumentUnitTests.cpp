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
#include "ArgMap.h"
#include "ArgumentValidation.h"
#include "ImageService.h"
#include "JsonUtils.h"
#include "Exceptions.h"
#include <chrono>
#include <wslc.h>

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIArgumentUnitTests {
namespace mount = wsl::windows::common::mount;

using RawArgMapBase = EnumBasedVariantMap<ArgType, wsl::windows::wslc::argument::details::ArgDataMapping, &ArgMapInvalidateValidatedCache>;

static_assert(!std::is_convertible_v<ArgMap*, RawArgMapBase*>);
static_assert(!std::is_copy_assignable_v<ArgMap>);
static_assert(!std::is_move_assignable_v<ArgMap>);

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

    TEST_METHOD(ArgumentException_OptionalArgumentHelp)
    {
        const ArgumentException withoutArgument{L"error"};
        VERIFY_IS_TRUE(withoutArgument.Arguments().empty());

        const auto argument = Argument::Create(ArgType::Verbose);
        const ArgumentException withArgument{L"error", argument};
        VERIFY_ARE_EQUAL(1u, withArgument.Arguments().size());
        VERIFY_ARE_EQUAL(ArgType::Verbose, withArgument.Arguments().front().Type());
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

        // Verify image pull policy
        auto pullPolicy = validation::GetPullPolicyFromString(L"always");
        VERIFY_ARE_EQUAL(pullPolicy, PullPolicy::Always);
        pullPolicy = validation::GetPullPolicyFromString(L"missing");
        VERIFY_ARE_EQUAL(pullPolicy, PullPolicy::Missing);
        pullPolicy = validation::GetPullPolicyFromString(L"never");
        VERIFY_ARE_EQUAL(pullPolicy, PullPolicy::Never);
        VERIFY_THROWS(validation::GetPullPolicyFromString(L"invalid"), ArgumentException);

        // Verify build progress mode
        VERIFY_ARE_EQUAL(validation::GetProgressModeFromString(L"auto"), ProgressMode::Auto);
        VERIFY_ARE_EQUAL(validation::GetProgressModeFromString(L"tty"), ProgressMode::Tty);
        VERIFY_ARE_EQUAL(validation::GetProgressModeFromString(L"plain"), ProgressMode::Plain);
        VERIFY_ARE_EQUAL(validation::GetProgressModeFromString(L"quiet"), ProgressMode::Quiet);
        VERIFY_THROWS(validation::GetProgressModeFromString(L"TTY"), ArgumentException); // Case-sensitive: only lowercase accepted
        VERIFY_THROWS(validation::GetProgressModeFromString(L"fancy"), ArgumentException);

        // Verify Docker-style memory size conversion.
        VERIFY_ARE_EQUAL(static_cast<int64_t>(1'610'612'736), validation::GetMemorySizeFromString(L"1.5G"));
        VERIFY_ARE_EQUAL(static_cast<int64_t>(314'572), validation::GetMemorySizeFromString(L"0.3MiB"));
        VERIFY_ARE_EQUAL(static_cast<int64_t>(32), validation::GetMemorySizeFromString(L"32.3"));
        VERIFY_ARE_EQUAL(static_cast<int64_t>(9'007'199'254'740'993), validation::GetMemorySizeFromString(L"9007199254740993"));
        VERIFY_ARE_EQUAL(std::numeric_limits<int64_t>::max(), validation::GetMemorySizeFromString(L"9223372036854775807"));
        VERIFY_THROWS(validation::GetMemorySizeFromString(L"-1.5G"), ArgumentException);
        VERIFY_THROWS(validation::GetMemorySizeFromString(L"9223372036854775808"), ArgumentException);

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
        auto retrievedBool = argsContainer.GetValue<ArgType::Help>();
        VERIFY_ARE_EQUAL(retrievedBool, true);
        auto retrievedString = argsContainer.GetValue<ArgType::ContainerId>();
        VERIFY_ARE_EQUAL(retrievedString, std::wstring(L"test"));
        auto retrievedStringSet = argsContainer.GetValue<ArgType::ForwardArgs>();
        VERIFY_ARE_EQUAL(retrievedStringSet[0], std::wstring(L"test1"));
        VERIFY_ARE_EQUAL(retrievedStringSet[1], std::wstring(L"test2"));

        // Verify multimap functionality and Runtime Add
        argsContainer.Add(ArgType::Publish, std::wstring(L"test1"));
        argsContainer.Add(ArgType::Publish, std::wstring(L"test2"));
        argsContainer.Add(ArgType::Publish, std::wstring(L"test3"));
        VERIFY_ARE_EQUAL(argsContainer.Count(ArgType::Publish), 3);
        auto publishArgs = argsContainer.GetAllValues<ArgType::Publish>();
        VERIFY_ARE_EQUAL(publishArgs.size(), 3);
        VERIFY_ARE_EQUAL(publishArgs[0], std::wstring(L"test1"));
        VERIFY_ARE_EQUAL(publishArgs[1], std::wstring(L"test2"));
        VERIFY_ARE_EQUAL(publishArgs[2], std::wstring(L"test3"));

        // Verify Remove
        ArgMap removeArgs;
        removeArgs.Add<ArgType::Publish>(L"test");
        removeArgs.Remove(ArgType::Publish);
        VERIFY_ARE_EQUAL(removeArgs.Count(ArgType::Publish), 0);

        // Verify compile time add works like runtime add for multimap types.
        ArgMap compileTimeArgs;
        compileTimeArgs.Add<ArgType::Publish>(L"test1");
        compileTimeArgs.Add<ArgType::Publish>(L"test2");
        compileTimeArgs.Add<ArgType::Publish>(L"test3");
        VERIFY_ARE_EQUAL(compileTimeArgs.Count(ArgType::Publish), 3);
        publishArgs = compileTimeArgs.GetAllValues<ArgType::Publish>();
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

        // An absent argument resolves to its value type's default-constructed value.
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::Memory));
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Memory), static_cast<size_t>(0));
        VERIFY_ARE_EQUAL(args.GetValue<ArgType::Memory>(), int64_t{});
        VERIFY_IS_FALSE(args.Contains(ArgType::Memory));
    }

    // Helper: run validation for a single-value argument and return the converted result (type fixed
    // by the argument's ConvertedType). Drives both paths for every converted ArgType its callers
    // exercise: the eager path (an explicit validation pass) and the on-demand path (a converted read
    // with no prior validation pass, which must self-validate). The returned value is the on-demand
    // result, so callers' expected-value assertions verify the on-demand output equals what the
    // validation pass produces. Both paths run the same Argument::Validate, so their results match by
    // construction; this asserts the on-demand trigger fires and caches an equal number of values.
    template <ArgType E>
    static auto ValidateAndGetCached(const std::wstring& raw)
    {
        ArgMap eager;
        eager.Add(E, std::wstring(raw));
        Argument::Create(E).Validate(eager);
        VERIFY_IS_TRUE(eager.ContainsValidated(E));

        ArgMap onDemand;
        onDemand.Add(E, std::wstring(raw));
        VERIFY_IS_FALSE(onDemand.ContainsValidated(E)); // no validation pass ran
        auto value = onDemand.GetValue<E>();            // triggers on-demand validation
        VERIFY_IS_TRUE(onDemand.ContainsValidated(E));
        VERIFY_ARE_EQUAL(onDemand.CountValidated(E), eager.CountValidated(E));
        return value;
    }

    // Helper: as ValidateAndGetCached, for an argument that appears multiple times (ArgMap is a
    // multimap). Runs the eager and on-demand paths and returns every on-demand converted value in
    // insertion order.
    template <ArgType E>
    static auto ValidateAndGetAllCached(const std::vector<std::wstring>& raws)
    {
        ArgMap eager;
        for (const auto& raw : raws)
        {
            eager.Add(E, std::wstring(raw));
        }

        Argument::Create(E).Validate(eager);

        // The cache must hold exactly one converted value per raw value in the map.
        VERIFY_ARE_EQUAL(eager.CountValidated(E), eager.Count(E));
        VERIFY_ARE_EQUAL(eager.CountValidated(E), raws.size());

        ArgMap onDemand;
        for (const auto& raw : raws)
        {
            onDemand.Add(E, std::wstring(raw));
        }

        VERIFY_IS_FALSE(onDemand.ContainsValidated(E)); // no validation pass ran
        auto values = onDemand.GetAllValues<E>();       // triggers on-demand validation
        VERIFY_ARE_EQUAL(onDemand.CountValidated(E), eager.CountValidated(E));
        return values;
    }

    // Test: Every ArgType whose validation converts its raw string into a typed value must cache
    // that value on the ArgMap during Argument::Validate, so execution reads it back without
    // re-converting. This drives the real validation + caching path for each converted ArgType.
    TEST_METHOD(ArgumentValidate_ConvertsAndCachesEveryConvertedArgType)
    {
        // string -> FormatType
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Format>(L"json"), FormatType::Json);

        // string -> json::dump() indentation
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::InspectFormat>(L"json"), wsl::shared::c_jsonCompactIndent);

        // string -> PullPolicy
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Pull>(L"missing"), PullPolicy::Missing);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Pull>(L"always"), PullPolicy::Always);
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Pull>(L"never"), PullPolicy::Never);

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
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::ShmSize>(L"1.5G"), static_cast<int64_t>(1'610'612'736));

        // string -> int64_t (durations, in nanoseconds)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthInterval>(L"30s"), validation::GetDurationNanosFromString(L"30s"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthTimeout>(L"30s"), validation::GetDurationNanosFromString(L"30s"));
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::HealthStartPeriod>(L"30s"), validation::GetDurationNanosFromString(L"30s"));

        // string -> int64_t (nano CPUs)
        VERIFY_ARE_EQUAL(ValidateAndGetCached<ArgType::Cpus>(L"1.5"), validation::GetNanoCpusFromString(L"1.5"));

        // mount strings -> mount::Spec
        {
            const auto volume = ValidateAndGetCached<ArgType::Volume>(LR"(C:\hostPath:/containerPath)");
            VERIFY_ARE_EQUAL(static_cast<int>(WSLCMountTypeBind), static_cast<int>(volume.MountType));
            VERIFY_ARE_EQUAL(static_cast<int>(mount::BindSourcePolicy::CreateIfMissing), static_cast<int>(volume.BindSource));

            const auto tmpfs = ValidateAndGetCached<ArgType::TMPFS>(L"/tmp:size=64k");
            VERIFY_ARE_EQUAL(static_cast<int>(WSLCMountTypeTmpfs), static_cast<int>(tmpfs.MountType));
            VERIFY_IS_TRUE(tmpfs.TmpfsOptions.has_value());
            VERIFY_ARE_EQUAL(std::string("size=64k"), tmpfs.TmpfsOptions.value());

            const auto structured = ValidateAndGetCached<ArgType::Mount>(L"type=volume,source=data-volume,target=/data");
            VERIFY_ARE_EQUAL(static_cast<int>(WSLCMountTypeVolume), static_cast<int>(structured.MountType));
            VERIFY_ARE_EQUAL(std::wstring(L"data-volume"), structured.Source);
        }

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

        // string -> BuildOutput (docker-style build exporter spec)
        {
            auto output = ValidateAndGetCached<ArgType::BuildOutput>(L"type=tar,dest=-");
            VERIFY_ARE_EQUAL(output.Type, std::wstring(L"tar"));
            VERIFY_ARE_EQUAL(output.Dest, std::wstring(L"-"));
        }

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

        // string -> ParsedNetworkArgument (docker-style network name and aliases)
        {
            auto network = ValidateAndGetCached<ArgType::Network>(L"name=custom,alias=web");
            VERIFY_ARE_EQUAL(network.Name, std::string("custom"));
            VERIFY_ARE_EQUAL(network.Aliases.size(), static_cast<size_t>(1));
            VERIFY_ARE_EQUAL(network.Aliases[0], std::string("web"));
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
            {ArgType::WorkDir, L"/app"},
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

    // Note: on-demand validation for every converted ArgType (reading with no prior validation pass
    // and getting the same result the pass produces) is covered by the tests above:
    // ValidateAndGetCached / ValidateAndGetAllCached drive both the eager and on-demand paths and
    // return the on-demand value, so those tests' expected-value assertions verify on-demand output
    // for all converted shapes. The tests below cover the behaviors unique to the on-demand trigger:
    // a bad value fails the same way as on the command line, a value added after the validation pass
    // is re-validated, and validate-only arguments (no converted value) are checked on demand too.

    // Test: An invalid value read on demand (no prior validation pass) throws ArgumentException --
    // the same failure the up-front validation pass raises for that value. This proves an argument
    // populated during execution routes to the same user error path as a bad command-line value,
    // and that a failed on-demand validation leaves nothing cached.
    TEST_METHOD(ArgumentValidate_OnDemandInvalidValueThrows)
    {
        ArgMap args;
        args.Add(ArgType::Format, std::wstring(L"xml")); // not a valid FormatType
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::Format));
        VERIFY_THROWS(args.GetValue<ArgType::Format>(), ArgumentException);
        VERIFY_IS_FALSE(args.ContainsValidated(ArgType::Format));
    }

    // Test: Raw values can change after the up-front validation pass until the argument is read.
    // The mutation invalidates the cache, and the first read validates the final values.
    TEST_METHOD(ArgumentValidate_PostValidationAddBeforeReadRevalidates)
    {
        ArgMap args;
        args.Add(ArgType::Signal, std::wstring(L"SIGTERM"));
        Argument::Create(ArgType::Signal).Validate(args);
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Signal), static_cast<size_t>(1));

        // Add a second raw value before the first read. The map-action callback drops the cache.
        args.Add(ArgType::Signal, std::wstring(L"SIGKILL"));
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Signal), static_cast<size_t>(0));

        // The first read re-validates both raw values on demand, in insertion order.
        auto signals = args.GetAllValues<ArgType::Signal>();
        VERIFY_ARE_EQUAL(signals.size(), static_cast<size_t>(2));
        VERIFY_ARE_EQUAL(signals[0], WSLCSignalSIGTERM);
        VERIFY_ARE_EQUAL(signals[1], WSLCSignalSIGKILL);
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Signal), static_cast<size_t>(2));
    }

    // Test: Arguments are validated on demand when read, so a value added after the up-front pass is
    // checked exactly as a command-line value.
    TEST_METHOD(ArgumentValidate_OnDemandArgIsChecked)
    {
        ArgMap labels;
        labels.Add(ArgType::BuildLabel, std::wstring(L"foo"));
        labels.Add(ArgType::BuildLabel, std::wstring(L"foo="));
        auto labelValues = labels.GetAllValues<ArgType::BuildLabel>();
        VERIFY_ARE_EQUAL(labelValues.size(), static_cast<size_t>(2));
        VERIFY_ARE_EQUAL(labelValues[0], std::wstring(L"foo"));
        VERIFY_ARE_EQUAL(labelValues[1], std::wstring(L"foo="));

        ArgMap invalidLabel;
        invalidLabel.Add(ArgType::BuildLabel, std::wstring(L"=value"));
        VERIFY_THROWS(invalidLabel.GetAllValues<ArgType::BuildLabel>(), wil::ResultException);

        // Valid value, no prior validation pass: the read validates and converts on demand.
        ArgMap valid;
        valid.Add(ArgType::Network, std::wstring(L"name=custom,alias=web"));
        auto networks = valid.GetAllValues<ArgType::Network>();
        VERIFY_ARE_EQUAL(networks.size(), static_cast<size_t>(1));
        VERIFY_ARE_EQUAL(networks[0].Name, std::string("custom"));
        VERIFY_ARE_EQUAL(networks[0].Aliases.size(), static_cast<size_t>(1));
        VERIFY_ARE_EQUAL(networks[0].Aliases[0], std::string("web"));

        // Invalid value, no prior validation pass: the read validates on demand and throws, matching
        // the failure the up-front pass raises for the same value.
        ArgMap invalid;
        invalid.Add(ArgType::Network, std::wstring(L"host"));
        VERIFY_THROWS(invalid.GetAllValues<ArgType::Network>(), ExecutionException);

        // Valid up-front, then an unsupported value added before the first read: the map-action
        // callback clears the validated record, so the read re-validates on demand and throws.
        ArgMap added;
        added.Add(ArgType::Network, std::wstring(L"bridge"));
        Argument::Create(ArgType::Network).Validate(added);
        added.Add(ArgType::Network, std::wstring(L"host"));
        VERIFY_THROWS(added.GetAllValues<ArgType::Network>(), ExecutionException);
    }

    TEST_METHOD(ArgumentValidate_ReadMakesArgumentImmutable)
    {
        ArgMap args;
        args.Add(ArgType::Signal, std::wstring(L"SIGTERM"));
        VERIFY_ARE_EQUAL(args.GetValue<ArgType::Signal>(), WSLCSignalSIGTERM);
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Signal), static_cast<size_t>(1));

        Argument::Create(ArgType::Signal).Validate(args);
        args.MarkValidated(ArgType::Signal);
        VERIFY_ARE_EQUAL(args.CountValidated(ArgType::Signal), static_cast<size_t>(1));

        const auto verifyImmutableFailure = [](const auto& operation) {
            VERIFY_THROWS_SPECIFIC(operation(), wil::ResultException, [](const wil::ResultException& e) {
                return e.GetErrorCode() == E_ILLEGAL_METHOD_CALL;
            });
        };

        verifyImmutableFailure([&] { args.Add(ArgType::Signal, std::wstring(L"SIGKILL")); });
        verifyImmutableFailure([&] { args.Remove(ArgType::Signal); });
        verifyImmutableFailure([&] { args.InvalidateValidated(ArgType::Signal); });
        verifyImmutableFailure([&] { args.AddValidated<ArgType::Signal>(WSLCSignalSIGKILL); });

        // Immutability is per argument; other arguments remain writable until they are read.
        args.Add(ArgType::StopTimeout, std::wstring(L"30"));
        VERIFY_ARE_EQUAL(args.GetValue<ArgType::StopTimeout>(), 30);
    }

    TEST_METHOD(ArgumentValidate_FlagReadValidatesAndMakesArgumentImmutable)
    {
        const auto verifyImmutableFailure = [](const auto& operation) {
            VERIFY_THROWS_SPECIFIC(operation(), wil::ResultException, [](const wil::ResultException& e) {
                return e.GetErrorCode() == E_ILLEGAL_METHOD_CALL;
            });
        };

        ArgMap absent;
        VERIFY_IS_FALSE(absent.GetValue<ArgType::Quiet>());
        VERIFY_IS_FALSE(absent.GetValue<ArgType::Quiet>(true));
        VERIFY_IS_FALSE(absent.GetValue<ArgType::Quiet>());
        VERIFY_IS_FALSE(absent.Contains(ArgType::Quiet));
        verifyImmutableFailure([&] { absent.Add(ArgType::Quiet, true); });

        ArgMap present;
        present.Add(ArgType::NoHealthcheck, true);
        present.Add(ArgType::HealthCmd, std::wstring(L"CMD echo healthy"));
        VERIFY_THROWS(present.GetValue<ArgType::NoHealthcheck>(), ArgumentException);

        // A failed read does not freeze the argument, so correcting the conflicting input permits
        // a subsequent successful read.
        present.Remove(ArgType::HealthCmd);
        VERIFY_IS_TRUE(present.GetValue<ArgType::NoHealthcheck>());
        verifyImmutableFailure([&] { present.Remove(ArgType::NoHealthcheck); });
    }

    // Timestamp parsing unit tests (exercises TryParseRfc3339 and integer path via GetTimestampFromString)

    TEST_METHOD(ValidateTimestamp_ValidUnixEpochSeconds)
    {
        // Integer timestamps should parse directly
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"0"), 0LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1700000000"), 1700000000LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1"), 1LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"9999999999"), 9999999999LL);
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_UTC)
    {
        // Basic UTC timestamps with Z suffix
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00Z"), 1705314600LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1970-01-01T00:00:00Z"), 0LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00z"), 1705314600LL); // lowercase z
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_WithOffset)
    {
        // Timestamps with timezone offsets (+HH:MM / -HH:MM)
        // 2024-01-15T10:30:00+05:30 = 2024-01-15T05:00:00Z = 1705294800
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00+05:30"), 1705294800LL);
        // 2024-01-15T10:30:00-05:00 = 2024-01-15T15:30:00Z = 1705332600
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00-05:00"), 1705332600LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00+00:00"), 1705314600LL);
    }

    TEST_METHOD(ValidateTimestamp_ValidRfc3339_FractionalSeconds)
    {
        // Fractional seconds should be consumed (truncated to seconds)
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.123Z"), 1705314600LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.123456789Z"), 1705314600LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.1+05:30"), 1705294800LL);
    }

    TEST_METHOD(ValidateTimestamp_ValidPreEpoch)
    {
        // No lower bound is applied, so pre-1970 values convert to a negative epoch.
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1960-01-15T10:30:00Z"), -314371800LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"0001-01-01T00:00:00Z"), -62135596800LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"-314371800"), -314371800LL);
    }

    TEST_METHOD(ValidateTimestamp_OutsideNanosecondRange)
    {
        // Values beyond the range of a nanosecond representation still convert exactly.
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"1600-01-01T00:00:00Z"), -11676096000LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2300-01-01T00:00:00Z"), 10413792000LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"9999-12-31T23:59:59Z"), 253402300799LL);
    }

    TEST_METHOD(ValidateTimestamp_ValidZoneLessLocalTime)
    {
        // A value with no zone designator is resolved against the local UTC offset.
        const auto offset = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::current_zone()->get_info(std::chrono::system_clock::now()).offset)
                                .count();
        const auto expected = 1705314600LL - offset;

        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00"), expected);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30:00.123"), expected);
    }

    TEST_METHOD(ValidateTimestamp_ValidPartialAndDateOnly)
    {
        // Hour-only, minute-only and date-only values are padded out to a full time.
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15"), validation::GetTimestampFromString(L"2024-01-15T00:00:00"));
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10"), validation::GetTimestampFromString(L"2024-01-15T10:00:00"));
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30"), validation::GetTimestampFromString(L"2024-01-15T10:30:00"));

        // The same padding applies when an explicit zone is present.
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10Z"), 1705312800LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15T10:30Z"), 1705314600LL);
        VERIFY_ARE_EQUAL(validation::GetTimestampFromString(L"2024-01-15Z"), 1705276800LL);
    }

    TEST_METHOD(ValidateTimestamp_ValidGoDuration)
    {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        // Durations are measured back from the current time, so allow a small window for the clock
        // to advance while the test runs.
        auto verifyDuration = [&](LPCWSTR value, LONGLONG expectedOffset) {
            const auto parsed = validation::GetTimestampFromString(value);
            VERIFY_IS_GREATER_THAN_OR_EQUAL(parsed, now - expectedOffset);
            VERIFY_IS_LESS_THAN_OR_EQUAL(parsed, now - expectedOffset + 30);
        };

        verifyDuration(L"10m", 600LL);
        verifyDuration(L"1h30m", 5400LL);
        verifyDuration(L"90s", 90LL);
        verifyDuration(L"1.5h", 5400LL);
        verifyDuration(L"2h45m30s", 9930LL);

        // A negative duration selects a time in the future.
        verifyDuration(L"-1h", -3600LL);
    }

    TEST_METHOD(ValidateTimestamp_SubSecondGoDuration)
    {
        // A sub-second duration is applied before the value is truncated, so a negative one still
        // resolves at or after the current second rather than being rounded away.
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        const auto future = validation::GetTimestampFromString(L"-500ms");
        VERIFY_IS_GREATER_THAN_OR_EQUAL(future, now);
        VERIFY_IS_LESS_THAN_OR_EQUAL(future, now + 30);

        const auto past = validation::GetTimestampFromString(L"500ms");
        VERIFY_IS_GREATER_THAN_OR_EQUAL(past, now - 1);
        VERIFY_IS_LESS_THAN_OR_EQUAL(past, now + 30);
    }

    TEST_METHOD(ValidateTimestamp_InvalidRfc3339_Rejected)
    {
        // Invalid month
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-13-15T10:30:00Z"), ArgumentException);
        // Invalid hour
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T25:30:00Z"), ArgumentException);
        // Invalid day (Feb 31)
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-02-31T10:30:00Z"), ArgumentException);
        // Trailing characters
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00Zextra"), ArgumentException);
        // +HHMM without colon (not supported by %Ez)
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00+0530"), ArgumentException);
        // Dot with no fractional digits
        VERIFY_THROWS(validation::GetTimestampFromString(L"2024-01-15T10:30:00.Z"), ArgumentException);
        // Random text
        VERIFY_THROWS(validation::GetTimestampFromString(L"abc"), ArgumentException);
        VERIFY_THROWS(validation::GetTimestampFromString(L"not-a-timestamp"), ArgumentException);
        // Duration with no unit
        VERIFY_THROWS(validation::GetTimestampFromString(L"10x"), ArgumentException);
        VERIFY_THROWS(validation::GetTimestampFromString(L"1h30"), ArgumentException);
    }
};
} // namespace WSLCCLIArgumentUnitTests
