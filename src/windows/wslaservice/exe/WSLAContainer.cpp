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

// Constants for required default arguments for "nerdctl run..."
static std::vector<std::string> defaultNerdctlRunArgs{//"--pull=never", // TODO: Uncomment once PullImage() is implemented.
                                                      "--net=host", // TODO: default for now, change later
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
    std::lock_guard lock{m_lock};

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
{
    return E_NOTIMPL;
}

HRESULT WSLAContainer::Delete()
try
{
    std::lock_guard lock{m_lock};

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
    std::lock_guard lock{m_lock};

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
    std::lock_guard lock{m_lock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_containerProcess.has_value());
    return m_containerProcess->Get().QueryInterface(__uuidof(IWSLAProcess), (void**)Process);
}
CATCH_RETURN();

HRESULT WSLAContainer::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    *Errno = -1;
    THROW_HR_IF_MSG(E_INVALIDARG, Options->Executable != nullptr, "Executable must be null");

    std::lock_guard lock{m_lock};

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        State() != WslaContainerStateRunning,
        "Container %hs is not running. State: %i",
        m_name.c_str(),
        m_state);

    auto [hasStdin, hasTty] = ParseFdStatus(*Options);

    std::vector<std::string> args{nerdctlPath, "exec"};

    if (hasStdin)
    {
        args.push_back("-i");
    }

    if (hasTty)
    {
        args.push_back("-t");
    }

    AddEnvironmentVariables(args, *Options);

    args.emplace_back(m_id);

    for (ULONG i = 0; i < Options->CommandLineCount; i++)
    {
        args.emplace_back(Options->CommandLine[i]);
    }

    ServiceProcessLauncher launcher(nerdctlPath, args, {}, common::ProcessFlags::None);
    for (auto i = 0; i < Options->FdsCount; i++)
    {
        launcher.AddFd(Options->Fds[i]);
    }

    std::optional<ServiceRunningProcess> process;
    HRESULT result = E_FAIL;
    std::tie(result, *Errno, process) = launcher.LaunchNoThrow(*m_parentVM);
    THROW_IF_FAILED(result);

    THROW_IF_FAILED(process->Get().QueryInterface(__uuidof(IWSLAProcess), (void**)Process));

    return S_OK;
}
CATCH_RETURN();

std::pair<bool, bool> WSLAContainer::ParseFdStatus(const WSLA_PROCESS_OPTIONS& Options)
{
    bool hasStdin = false;
    bool hasTty = false;
    for (size_t i = 0; i < Options.FdsCount; i++)
    {
        if (Options.Fds[i].Fd == 0)
        {
            hasStdin = true;
        }

        if (Options.Fds[i].Type == WSLAFdTypeTerminalInput || Options.Fds[i].Type == WSLAFdTypeTerminalOutput)
        {
            hasTty = true;
        }
    }

    return {hasStdin, hasTty};
}

void WSLAContainer::AddEnvironmentVariables(std::vector<std::string>& args, const WSLA_PROCESS_OPTIONS& options)
{
    for (ULONG i = 0; i < options.EnvironmentCount; i++)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, options.Environment[i][0] == '-', "Invalid environment string: %hs", options.Environment[i]);

        args.insert(args.end(), {"-e", options.Environment[i]});
    }
}

Microsoft::WRL::ComPtr<WSLAContainer> WSLAContainer::Create(
    const WSLA_CONTAINER_OPTIONS& containerOptions, WSLAVirtualMachine& parentVM, ContainerEventTracker& eventTracker)
{
    auto [hasStdin, hasTty] = ParseFdStatus(containerOptions.InitProcessOptions);

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

    AddEnvironmentVariables(inputOptions, containerOptions.InitProcessOptions);

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

    args.insert(args.end(), defaultNerdctlRunArgs.begin(), defaultNerdctlRunArgs.end());
    args.insert(args.end(), inputOptions.begin(), inputOptions.end());

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
