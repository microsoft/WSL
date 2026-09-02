// Copyright (C) Microsoft Corporation. All rights reserved.

// libFuzzer harness for the WSLC CLI argument parser (ParseArgumentsStateMachine).
//
// This target exercises the state machine that parses all `wslc.exe` command-line
// arguments. It is fully standalone — no COM session or WSL service is required.

#include "precomp.h"

#include "arguments/Argument.h"
#include "arguments/ArgumentParser.h"
#include "arguments/ArgumentTypes.h"
#include "core/Invocation.h"
#include "FuzzingHarness.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

// Split remaining fuzz bytes into null-delimited wide strings (argv-style).
static std::vector<std::wstring> SplitToArgs(FuzzInput& input)
{
    std::vector<std::wstring> args;
    while (!input.Empty())
    {
        auto s = input.ReadWideString(SIZE_MAX);
        if (!s.empty())
        {
            args.push_back(std::move(s));
        }
    }
    return args;
}

// Build a representative set of arguments covering all Kinds (Flag, Value, Positional, Forward).
static std::vector<Argument> GetFuzzArguments()
{
    return {
        Argument::Create(ArgType::Help),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::Detach),
        Argument::Create(ArgType::Force),
        Argument::Create(ArgType::Interactive),
        Argument::Create(ArgType::Quiet),
        Argument::Create(ArgType::TTY),
        Argument::Create(ArgType::Verbose),
        Argument::Create(ArgType::Version),
        Argument::Create(ArgType::Name),
        Argument::Create(ArgType::Env, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Volume, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Publish, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::WorkDir),
        Argument::Create(ArgType::User),
        Argument::Create(ArgType::Hostname),
        Argument::Create(ArgType::Memory),
        Argument::Create(ArgType::Signal),
        Argument::Create(ArgType::Output),
        Argument::Create(ArgType::ContainerId),
        Argument::Create(ArgType::ImageId),
        Argument::Create(ArgType::Command),
        Argument::Create(ArgType::ForwardArgs),
    };
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096)
    {
        return -1;
    }

    try
    {
        FuzzInput input{data, size};
        auto args = SplitToArgs(input);
        if (args.empty())
        {
            return -1;
        }

        Invocation inv{std::move(args)};
        ArgMap execArgs;
        auto arguments = GetFuzzArguments();

        ParseArgumentsStateMachine sm{inv, execArgs, std::move(arguments)};

        while (sm.Step())
        {
        }
    }
    catch (...)
    {
        // Expected — most fuzzed inputs will fail. ASAN crashes bypass this.
    }

    return 0;
}
