/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.cpp

Abstract:

    Contains the implementation of WSLAContainer.

--*/

#include "precomp.h"
#include "WSLAContainer.h"
#include "WSLAProcess.h"

using wsl::windows::service::wsla::WSLAContainer;

constexpr const char* nerdctlPath = "/usr/bin/nerdctl";

// Constants for required default arguments for "nerdctl run..."
static std::vector<std::string> defaultNerdctlRunArgs{//"--pull=never", // TODO: Uncomment once PullImage() is implemented.
                                                      "--net=host", // TODO: default for now, change later
                                                      "--ulimit",
                                                      "nofile=65536:65536"};

HRESULT WSLAContainer::Start()
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Stop(int Signal, ULONG TimeoutMs)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Delete()
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::GetState(WSLA_CONTAINER_STATE* State)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::GetInitProcess(IWSLAProcess** Process)
try
{
    return m_containerProcess.Get().QueryInterface(__uuidof(IWSLAProcess), (void**)Process);
}
CATCH_RETURN();

HRESULT WSLAContainer::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    // auto process = wil::MakeOrThrow<WSLAProcess>();

    // process.CopyTo(__uuidof(IWSLAProcess), (void**)Process);

    return S_OK;
}
CATCH_RETURN();

Microsoft::WRL::ComPtr<WSLAContainer> WSLAContainer::Create(const WSLA_CONTAINER_OPTIONS& containerOptions, WSLAVirtualMachine& parentVM)
{
    // TODO: Switch to nerdctl create, and call nerdctl start in Start().

    bool hasStdin = false;
    bool hasTty = false;
    for (size_t i = 0; i < containerOptions.InitProcessOptions.FdsCount; i++)
    {
        if (containerOptions.InitProcessOptions.Fds[i].Fd == 0)
        {
            hasStdin = true;
        }

        if (containerOptions.InitProcessOptions.Fds[i].Type == WSLAFdTypeTerminalInput ||
            containerOptions.InitProcessOptions.Fds[i].Type == WSLAFdTypeTerminalOutput)
        {
            hasTty = true;
        }
    }

    std::vector<std::string> inputOptions;
    if (hasStdin)
    {
        // For now return a proper error if the caller tries to pass stdin without a TTY to prevent hangs.
        THROW_WIN32_IF(ERROR_NOT_SUPPORTED, hasTty == false);
        inputOptions.push_back("-i");
    }

    if (hasTty)
    {
        inputOptions.push_back("-t");
    }

    auto args = PrepareNerdctlRunCommand(containerOptions, std::move(inputOptions));

    ServiceProcessLauncher launcher(nerdctlPath, args, {}, common::ProcessFlags::None);
    for (size_t i = 0; i < containerOptions.InitProcessOptions.FdsCount; i++)
    {
        launcher.AddFd(containerOptions.InitProcessOptions.Fds[i]);
    }

    return wil::MakeOrThrow<WSLAContainer>(&parentVM, launcher.Launch(parentVM));
}

std::vector<std::string> WSLAContainer::PrepareNerdctlRunCommand(const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions)
{
    std::vector<std::string> args{nerdctlPath};
    args.push_back("run");
    args.push_back("--name");
    args.push_back(options.Name);
    if (options.ShmSize > 0)
    {
        args.push_back("--shm-size=" + std::to_string(options.ShmSize) + 'm');
    }
    if (options.Flags & WSLA_CONTAINER_FLAG_ENABLE_GPU)
    {
        args.push_back("--gpus");
        // TODO: Parse GPU device list from WSLA_CONTAINER_OPTIONS. For now, just enable all GPUs.
        args.push_back("all");
    }

    args.insert(args.end(), defaultNerdctlRunArgs.begin(), defaultNerdctlRunArgs.end());
    args.insert(args.end(), inputOptions.begin(), inputOptions.end());

    for (ULONG i = 0; i < options.InitProcessOptions.EnvironmentCount; i++)
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            options.InitProcessOptions.Environment[i][0] == L'-',
            "Invlaid environment string: %hs",
            options.InitProcessOptions.Environment[i]);

        args.insert(args.end(), {"-e", options.InitProcessOptions.Environment[i]});
    }

    if (options.InitProcessOptions.Executable != nullptr)
    {
        args.push_back("--entrypoint");
        args.push_back(options.InitProcessOptions.Executable);
    }

    // TODO:
    // - Implement volume mounts
    // - Implement port mapping

    args.push_back(options.Image);

    if (options.InitProcessOptions.CommandLineCount > 0)
    {
        args.push_back("--");

        for (ULONG i = 0; i < options.InitProcessOptions.CommandLineCount; i++)
        {
            args.push_back(options.InitProcessOptions.CommandLine[i]);
        }
    }

    // TODO: Implement --entrypoint override if specified in WSLA_CONTAINER_OPTIONS.

    return args;
}