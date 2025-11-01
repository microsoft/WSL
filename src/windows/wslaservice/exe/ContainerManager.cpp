#include "C:/Users/trivedipooja/source/repos/WSL/src/windows/common/CMakeFiles/common.dir/Debug/cmake_pch.hxx"
#include "ContainerManager.h"
#include <string>
#include <sstream>
#include <vector>

namespace wsl::windows::service::wsla {

ContainerManager::ContainerManager(WSLAVirtualMachine* pVM) : m_pVM(pVM)
{
}

ContainerManager::~ContainerManager()
{
}

ContainerResult ContainerManager::StartNewContainer(const ContainerOptions& options)
{
    // TODO: Fix!
    std::string containerNameStr = "something";//Utils::GenerateUuidString();

    ContainerResult containerResult;
    int containerId = m_containerId++;

    {
        std::lock_guard<std::recursive_mutex> containersLock{m_containersLock};
        if (CheckPortConflicts(options.PortMappings))
        {
            THROW_WIN32_MSG(ERROR_ADDRESS_ALREADY_ASSOCIATED, "Port is already in use.");
        }

        m_containers.emplace(containerId, ContainerInfo{containerNameStr, ContainerState::Creating});
    }

    // Building nerdctl run command with args
    std::string command = prepareNerdctlRunCommand(options.Image, containerNameStr, options);

    try
    {
        auto runningProcess = StartProcess(command, options.InitProcessOptions);

        containerResult.Result = S_OK;
        containerResult.ContainerId = containerId;
        containerResult.MainProcess.StdIn = runningProcess.StdIn;
        containerResult.MainProcess.StdOut = runningProcess.StdOut;
        containerResult.MainProcess.StdErr = runningProcess.StdErr;
        containerResult.MainProcess.Pid = runningProcess.Pid;

        if (SUCCEEDED(containerResult.Result))
        {
            std::lock_guard<std::recursive_mutex> containersLock{m_containersLock};
            m_containers[containerId].State = ContainerState::Created;

            // Adding port mapping
            for (const auto& portMapping : options.PortMappings)
            {
                THROW_IF_FAILED(m_pVM->MapPort(portMapping.AddressFamily, portMapping.WindowsPort, portMapping.LinuxPort, false));
                m_containers[containerId].PortMappings.push_back(portMapping);
            }
        }
        return containerResult;
    }
    catch (const wil::ResultException& re)
    {
        // TODO: LOG_CAUGHT_EXCEPTION() when logging is enabled

        std::ignore = StopContainer(containerId, true);

        containerResult.Result = re.GetErrorCode();
        return containerResult;
    }
}

ContainerResult ContainerManager::StartContainer(const int containerId)
{
    ContainerResult containerResult;
    containerResult.Result = E_NOTIMPL; // TODO: Implement
    return containerResult;
}

HRESULT ContainerManager::StopContainer(const int containerId, bool remove)
{
    return S_OK;
    /*
    std::lock_guard<std::recursive_mutex> containersLock{m_containersLock};
    auto it = m_containers.find(containerId);
    THROW_HR_IF_MSG(E_INVALIDARG, it == m_containers.end(), "Container with id %d not found.", containerId);
    if (it->second.State != ContainerState::Started)
    {
        RETURN_HR(S_OK);
    }
    // Building nerdctl stop command with args
    std::vector<std::string> args{"stop", "--time=10", it->second.Name};
    auto runningProcess = StartProcess(args);
    auto processResult = WaitForProcess(runningProcess, 20000);
    RETURN_HR_IF_MSG(processResult.Result, FAILED(processResult.Result), "Failed to stop container %d", containerId);
    if (remove)
    {
        // Building nerdctl rm command with args
        std::vector<std::string> rmArgs{"rm", it->second.Name};
        auto rmProcess = StartProcess(rmArgs);
        auto rmProcessResult = WaitForProcess(rmProcess, 20000);
        RETURN_HR_IF_MSG(rmProcessResult.Result, FAILED(rmProcessResult.Result), "Failed to remove container %d", containerId);
    }
    // Unmapping ports
    for (const auto& portMapping : it->second.PortMappings)
    {
        THROW_IF_FAILED(m_pVM->MapPort(portMapping.AddressFamily, portMapping.WindowsPort, portMapping.LinuxPort, true));
    }
    m_containers.erase(it);
    RETURN_HR(S_OK);
    */
}

ContainerResult ContainerManager::RestartContainer(const int containerId)
{
    ContainerResult containerResult;
    containerResult.Result = E_NOTIMPL; // TODO: Implement
    return containerResult;
}

////// private

bool ContainerManager::CheckPortConflicts(const std::vector<PortMapping>& portMappings)
{
    for (const auto& newMapping : portMappings)
    {
        for (const auto& containerEntry : m_containers)
        {
            for (const auto& existingMapping : containerEntry.second.PortMappings)
            {
                if (newMapping.AddressFamily == existingMapping.AddressFamily &&
                    (newMapping.LinuxPort == existingMapping.LinuxPort || newMapping.WindowsPort == existingMapping.WindowsPort))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ContainerManager::IsContainerRunning(const int containerId)
{
    return false;
    /*
    std::lock_guard<std::recursive_mutex> containersLock{m_containersLock};
    auto it = m_containers.find(containerId);
    THROW_HR_IF_MSG(E_INVALIDARG, it == m_containers.end(), "Container with id %d not found.", containerId);
    return it->second.State == ContainerState::Started;
    */
}

std::string ContainerManager::prepareNerdctlRunCommand(std::string_view image, std::string_view containerName, const ContainerOptions& options)
{
    ContainerManager::NerdctlCommandBuilder builder;

    builder.addArgument("run").addArgument(ContainerManager::defaultNerdctlRunArgs).addArgument("--name").addArgument(containerName);
    if (options.ShmSizeMb > 0)
    {
        builder.addArgument("--shm-size=" + std::to_string(options.ShmSizeMb) + 'm');
    }
    if (options.GPUOptions.Enable)
    {
        builder.addArgument({"--gpus", options.GPUOptions.GPUDevices});
    }

    // TODO: Add envs!
    /* for (const auto& env : options.MainProcessOptions.Envs)
    {
        args.insert(args.end(), {"-e", env});
    } */

    // Adding local mount paths
    for (const auto& volume : options.Volumes)
    {
        THROW_WIN32_IF_MSG(
            ERROR_NOT_SUPPORTED, volume.MountPoint.find(':') != std::string::npos, "Char ':' not supported for MountPoint.");

        std::string mountContainerPath;
        mountContainerPath = std::string(volume.HostPath) + ":" + std::string(volume.MountPoint);
        if (volume.IsReadOnly)
        {
            mountContainerPath += ":ro";
        }

        builder.addArgument({"-v", mountContainerPath});
    }

    // TODO: .addEnv("SOMETHING1", "SOEMTHING2") // Add a custom environment variable
    builder.addArgument(image);

    // Add main process args
    for (const auto& processArgs : options.InitProcessOptions.CommandLine)
    {
        builder.addArgument(processArgs);
    }

    return builder.build();
}

ContainerProcess ContainerManager::StartProcess(const std::string& command, const ContainerProcessOptions& processOptions)
{
    WSLA_CREATE_PROCESS_OPTIONS options{};

    auto Count = [](const auto* Ptr) -> ULONG {
        if (Ptr == nullptr)
        {
            return 0;
        }

        ULONG Result = 0;

        while (*Ptr != nullptr)
        {
            Result++;
            Ptr++;
        }

        return Result;
    };

    std::vector<LPCSTR> clArray;
    for (const auto& cl : processOptions.CommandLine)
    {
        clArray.push_back(cl.c_str());
    }
    clArray.push_back(nullptr);

    std::vector<LPCSTR> envArray;
    for (const auto& env : processOptions.CommandLine)
    {
        envArray.push_back(env.c_str());
    }
    clArray.push_back(nullptr);
    options.Executable = processOptions.Executable.c_str();
    options.CommandLine = clArray.data();
    options.CommandLineCount = Count(options.CommandLine);
    options.Environment = envArray.data();
    options.EnvironmentCount = Count(options.Environment);
    options.CurrentDirectory = processOptions.CurrentDirectory.c_str();

    std::vector<WSLA_PROCESS_FD> inputFd(3);
    inputFd[0].Fd = 0;
    inputFd[0].Type = WslFdType::WslFdTypeDefault;
    inputFd[1].Fd = 1;
    inputFd[1].Type = WslFdType::WslFdTypeDefault;
    inputFd[2].Fd = 2;
    inputFd[2].Type = WslFdType::WslFdTypeDefault;
    
    std::vector<const char*> env{"PATH=/sbin:/usr/sbin:/bin:/usr/bin", nullptr};

    std::vector<ULONG> fds(3);
    if (fds.empty())
    {
        fds.resize(1); // COM doesn't like null pointers.
    }

    WSLA_CREATE_PROCESS_RESULT result{};
    THROW_IF_FAILED(reinterpret_cast<IWSLAVirtualMachine*>(m_pVM)
                         ->CreateLinuxProcess(&options, 3, inputFd.data(), fds.data(), &result));

    return { UlongToHandle(fds[0]), UlongToHandle(fds[1]), UlongToHandle(fds[2]), result.Pid };
}
} // namespace wsl::windows::service::wsla

