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

// Constants for required default arguments for "nerdctl create..."
static std::vector<std::string> defaultNerdctlCreateArgs{//"--pull=never", // TODO: Uncomment once PullImage() is implemented.
                                                         "--ulimit",
                                                         "nofile=65536:65536"};

WSLAContainer::WSLAContainer(WSLAVirtualMachine* parentVM, const WSLA_CONTAINER_OPTIONS& Options, std::string&& Id, ContainerEventTracker& tracker) :
    m_parentVM(parentVM), m_name(Options.Name), m_image(Options.Image), m_id(std::move(Id))
{
    m_state = WslaContainerStateCreated;

    m_trackingReference = tracker.RegisterContainerStateUpdates(m_id, std::bind(&WSLAContainer::OnEvent, this, std::placeholders::_1));
}

WSLAContainer::~WSLAContainer()
{
    m_trackingReference.Reset();
}

const std::string& WSLAContainer::Image() const noexcept
{
    return m_image;
}

void WSLAContainer::Start(const WSLA_CONTAINER_OPTIONS& Options)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateCreated,
        "Cannot start container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    ServiceProcessLauncher launcher(nerdctlPath, {nerdctlPath, "start", "-a", m_id}, {}, common::ProcessFlags::None);
    for (auto i = 0; i < Options.InitProcessOptions.FdsCount; i++)
    {
        launcher.AddFd(Options.InitProcessOptions.Fds[i]);
    }

    m_containerProcess = launcher.Launch(*m_parentVM);

    auto cleanup = wil::scope_exit([&]() { m_containerProcess.reset(); });

    // Wait for either the container to get into a 'started' state, or the nerdctl process to exit.
    common::relay::MultiHandleWait wait;
    wait.AddHandle(std::make_unique<common::relay::EventHandle>(m_containerProcess->GetExitEvent(), [&]() { wait.Cancel(); }));
    wait.AddHandle(std::make_unique<common::relay::EventHandle>(m_startedEvent.get(), [&]() { wait.Cancel(); }));
    wait.Run({});

    if (!m_startedEvent.is_signaled())
    {
        auto status = GetNerdctlStatus();

        THROW_HR_IF_MSG(
            E_FAIL,
            status != "exited",
            "Failed to start container %hs, nerdctl status: %hs",
            m_name.c_str(),
            status.value_or("<empty>").c_str());
    }

    cleanup.release();
    m_state = WslaContainerStateRunning;
}

void WSLAContainer::OnEvent(ContainerEvent event)
{
    if (event == ContainerEvent::Start)
    {
        m_startedEvent.SetEvent();
    }

    WSL_LOG(
        "ContainerEvent",
        TraceLoggingValue(m_name.c_str(), "Name"),
        TraceLoggingValue(m_id.c_str(), "Id"),
        TraceLoggingValue((int)event, "Event"));
}

HRESULT WSLAContainer::Stop(int Signal, ULONG TimeoutMs)
try
{
    std::lock_guard lock(m_lock);

    if (State() == WslaContainerStateExited)
    {
        return S_OK;
    }

    /* 'nerdctl stop ...'
     *   returns success and <containerId> on stdout if the container is running or already stopped
     *   returns error "No such container: <containerId>" on stderr if the container is in 'Created' state or does not exist
     *
     * For our case, we treat stopping an already-exited container as a no-op and return success.
     * Stopping a deleted or created container returns ERROR_INVALID_STATE.
     * TODO: Discuss and return stdout/stderr or corresponding HRESULT from nerdctl stop for better diagnostics.
     */

    // Validate that the container is in the running state.
    RETURN_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateRunning,
        "Container '%hs' is not in a stoppable state: %i",
        m_name.c_str(),
        m_state);
    ServiceProcessLauncher launcher(
        nerdctlPath, {nerdctlPath, "stop", m_name, "--time", std::to_string(static_cast<ULONG>(std::round(TimeoutMs / 1000)))});
    // TODO: Figure out how we want to handle custom signals.
    // nerdctl stop has a --time and a --signal option that can be used
    // By default, it uses SIGTERM and a default timeout of 10 seconds.
    auto result = launcher.Launch(*m_parentVM).WaitAndCaptureOutput();
    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());

    m_state = WslaContainerStateExited;
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::Delete()
try
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    // Validate that the container is in the exited state.
    RETURN_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        State() != WslaContainerStateExited,
        "Cannot delete container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    ServiceProcessLauncher launcher(nerdctlPath, {nerdctlPath, "rm", "-f", m_name});
    auto result = launcher.Launch(*m_parentVM).WaitAndCaptureOutput();
    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());

    m_state = WslaContainerStateDeleted;
    return S_OK;
}
CATCH_RETURN();

WSLA_CONTAINER_STATE WSLAContainer::State() noexcept
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    // If the container is running, refresh the init process state before returning.
    if (m_state == WslaContainerStateRunning && m_containerProcess->State() != WSLAProcessStateRunning)
    {
        m_state = WslaContainerStateExited;
        m_containerProcess.reset();
    }

    return m_state;
}

HRESULT WSLAContainer::GetState(WSLA_CONTAINER_STATE* Result)
try
{
    *Result = State();

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::GetInitProcess(IWSLAProcess** Process)
try
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_containerProcess.has_value());
    return m_containerProcess->Get().QueryInterface(__uuidof(IWSLAProcess), (void**)Process);
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

Microsoft::WRL::ComPtr<WSLAContainer> WSLAContainer::Create(
    const WSLA_CONTAINER_OPTIONS& containerOptions, WSLAVirtualMachine& parentVM, ContainerEventTracker& eventTracker)
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

    // Don't support stdin for now since it will hang.
    // TODO: Remove once stdin is fixed in nerdctl.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), hasStdin && !hasTty);

    std::vector<std::string> inputOptions;
    if (hasStdin)
    {
        inputOptions.push_back("-i");
    }

    if (hasTty)
    {
        inputOptions.push_back("-t");
    }

    auto args = PrepareNerdctlCreateCommand(containerOptions, std::move(inputOptions));

    ServiceProcessLauncher launcher(nerdctlPath, args, {});
    auto result = launcher.Launch(parentVM).WaitAndCaptureOutput();

    // TODO: Have better error codes.
    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "Failed to create container: %hs", launcher.FormatResult(result).c_str());

    auto id = result.Output[1];
    while (!id.empty() && (id.back() == '\n'))
    {
        id.pop_back();
    }

    return wil::MakeOrThrow<WSLAContainer>(&parentVM, containerOptions, std::move(id), eventTracker);
}

std::vector<std::string> WSLAContainer::PrepareNerdctlCreateCommand(const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions)
{
    std::vector<std::string> args{nerdctlPath};
    args.push_back("create");
    args.push_back("--name");
    args.push_back(options.Name);

    switch (options.ContainerNetwork.ContainerNetworkType)
    {
    case WSLA_CONTAINER_NETWORK_HOST:
        args.push_back("--net=host");
        break;
    case WSLA_CONTAINER_NETWORK_NONE:
        args.push_back("--net=none");
        break;
    case WSLA_CONTAINER_NETWORK_BRIDGE:
        args.push_back("--net=bridge");
        break;
    // TODO: uncomment and implement when we have custom networks
    // case WSLA_CONTAINER_NETWORK_CUSTOM:
    //     args.push_back(std::format("--net={}", options.ContainerNetwork.ContainerNetworkName));
    //     break;
    default:
        THROW_HR_MSG(
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            "No such network: type: %i, name: %hs",
            options.ContainerNetwork.ContainerNetworkType,
            options.ContainerNetwork.ContainerNetworkName);
        break;
    }

    if (options.ShmSize > 0)
    {
        args.push_back(std::format("--shm-size={}m", options.ShmSize));
    }
    if (options.Flags & WSLA_CONTAINER_FLAG_ENABLE_GPU)
    {
        args.push_back("--gpus");
        // TODO: Parse GPU device list from WSLA_CONTAINER_OPTIONS. For now, just enable all GPUs.
        args.push_back("all");
    }

    args.insert(args.end(), defaultNerdctlCreateArgs.begin(), defaultNerdctlCreateArgs.end());
    args.insert(args.end(), inputOptions.begin(), inputOptions.end());

    for (ULONG i = 0; i < options.InitProcessOptions.EnvironmentCount; i++)
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            options.InitProcessOptions.Environment[i][0] == L'-',
            "Invalid environment string: %hs",
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

    return args;
}

std::optional<std::string> WSLAContainer::GetNerdctlStatus()
{
    ServiceProcessLauncher launcher(nerdctlPath, {nerdctlPath, "inspect", "-f", "{{.State.Status}}", m_name});
    auto result = launcher.Launch(*m_parentVM).WaitAndCaptureOutput();
    if (result.Code != 0)
    {
        // Can happen if the container is not found.
        // TODO: Find a way to validate that the container is indeed not found, and not some other error.
        return {};
    }

    auto& status = result.Output[1];

    while (!status.empty() && status.back() == '\n')
    {
        status.pop_back();
    }

    // N.B. nerdctl inspect can return with exit code 0 and no output. Return an empty optional if that happens.
    return status.empty() ? std::optional<std::string>{} : status;
}