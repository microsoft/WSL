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

const std::string nerdctlPath = "/usr/bin/nerdctl";

// Constants for required default arguments for "nerdctl run..."
static std::vector<std::string> defaultNerdctlRunArgs{       //"--pull=never", // TODO: Uncomment once PullImage() is implemented.
                                                      "-it", // TODO: only enable if fds allow for a tty.
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
    auto args = WSLAContainer::prepareNerdctlRunCommand(containerOptions);

    ServiceProcessLauncher launcher(nerdctlPath, args, {}, common::ProcessFlags::None);
    for (size_t i = 0; i < containerOptions.InitProcessOptions.FdsCount; i++)
    {
        launcher.AddFd(containerOptions.InitProcessOptions.Fds[i]);
    }

    return wil::MakeOrThrow<WSLAContainer>(&parentVM, launcher.Launch(parentVM));
}

std::vector<std::string> WSLAContainer::prepareNerdctlRunCommand(const WSLA_CONTAINER_OPTIONS& options)
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
        // args.push_back(options.GPUOptions.GPUDevices);
    }

    args.insert(args.end(), defaultNerdctlRunArgs.begin(), defaultNerdctlRunArgs.end());

    for (ULONG i = 0; i < options.VolumesCount; i++)
    {
        std::string mountContainerPath;
        mountContainerPath = std::string(options.Volumes[i].HostPath) + ":" + std::string(options.Volumes[i].ContainerPath);
        if (options.Volumes[i].ReadOnly)
        {
            mountContainerPath += ":ro";
        }
        args.insert(args.end(), {"-v", mountContainerPath});
    }

    args.push_back(options.Image);

    if (options.InitProcessOptions.CommandLineCount)
    {
        args.push_back("--");
    }
    for (ULONG i = 0; i < options.InitProcessOptions.CommandLineCount; i++)
    {
        args.push_back(options.InitProcessOptions.CommandLine[i]);
    }

    return args;
}