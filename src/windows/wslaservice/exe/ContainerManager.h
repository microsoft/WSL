#pragma once

#include <memory>
#include <vector>
#include <string>
#include <filesystem>
#include <wil\resource.h>
#include <map>
#include <mutex>

#include "WSLAVirtualMachine.h"

namespace wsl::windows::service::wsla {

    struct ContainerVolume
    {
        bool IsReadOnly;
        std::string HostPath;
        std::string MountPoint;
    };

    struct PortMapping
    {
        uint16_t WindowsPort;
        uint16_t LinuxPort;
        int AddressFamily;
    };

    struct GPUOptions
    {
        bool Enable;
        std::string GPUDevices;
    };

    enum ContainerProcessFlags
    {
        None = 0,
        InteractiveShell,
    };

    struct ContainerProcessOptions
    {
        std::string Executable;
        std::vector<std::string> CommandLine;
        std::vector<std::string> Environment;
        std::string CurrentDirectory;
        HANDLE TerminalControlChannel; // Used to create interactive shells, (to handle terminal window resizes)
        uint32_t Rows; // Only applicable when creating an interactive shell
        uint32_t Columns;
        uint32_t Flags = None | InteractiveShell;
    }; 
        
    // For use when starting or waiting on a new nerdctl process
    struct ContainerProcess
    {
        HANDLE StdIn;
        HANDLE StdOut;
        HANDLE StdErr;
        int Pid = -1;
    };

    struct ContainerOptions
    {
        std::string Image;
        std::string Name;
        ContainerProcessOptions InitProcessOptions;
        std::vector<ContainerVolume> Volumes;
        std::vector<PortMapping> PortMappings;
        GPUOptions GPUOptions;
        uint64_t ShmSizeMb;
    };

    struct ContainerResult
    {
        HRESULT Result;
        int32_t ContainerId = -1;
        ContainerProcess MainProcess;
    };

    enum ContainerState
    {
        Default,
        Creating,
        Created,
        Stopping,
        Exited,
        Failed,

        // TODO: Future consideration, add more container states and keep
        // states in sync with actual runtime, like exited, etc.
    };

    // For use in the container map m_containers
    struct ContainerInfo
    {
        std::string Name;
        ContainerState State = ContainerState::Default;
        std::vector<PortMapping> PortMappings;
    };

    struct ContainerProcessResult
    {
        HRESULT Result;
        int ExitCode = -1;
        std::string StdOut;
        std::string StdErr;
    };

    class ContainerManager
    {
    public:
        ContainerManager(WSLAVirtualMachine* vm);
        ContainerManager(const ContainerManager&) = delete;
        ContainerManager& operator=(const ContainerManager&) = delete;
        ContainerManager(ContainerManager&&) = delete;
        ContainerManager& operator=(ContainerManager&&) = delete;

        ~ContainerManager();

        ContainerResult StartNewContainer(const ContainerOptions& options);
        ContainerResult StartContainer(const int containerId);
        HRESULT StopContainer(const int containerId, bool remove);
        ContainerResult RestartContainer(const int containerId);

        // Constants for required default arguments for "nerdctl run..."
        static constexpr std::initializer_list<std::string_view> defaultNerdctlRunArgs = {
            "--pull=never",
            "--host=net", // TODO: default for now, change later
            "--ulimit nofile=65536:65536"};


        // Nested helper class for building nerdctl commands
        class NerdctlCommandBuilder
        {
        public:
        private:
            const std::string baseCommand = "/usr/bin/nerdctl";
            std::vector<std::string_view> args;
            // TODO: std::vector<std::string> envVariables; // Store owned strings for ENV vars
            std::vector<std::string_view> defaultGlobalArgs; // Any nerdctl-wide global args we want added to every nerdctl command

        public:
            NerdctlCommandBuilder()
            {
                // Initialize with default arguments
                if (defaultGlobalArgs.size())
                {
                    args.insert(args.end(), defaultGlobalArgs.begin(), defaultGlobalArgs.end());
                }
            }

            NerdctlCommandBuilder& addArgument(std::string_view arg)
            {
                args.push_back(arg);
                return *this;
            }

            NerdctlCommandBuilder& addArgument(std::initializer_list<std::string_view> arguments)
            {
                // Efficiently insert all elements from the initializer list into the arguments vector
                args.insert(args.end(), arguments);
                return *this;
            }

            /* TODO : Adds environment variables(e.g., -e KEY = VALUE)
            NerdctlCommandBuilder& addEnv(const std::string& key, const std::string& value)
            {
                // Formats as "-e KEY=VALUE"
                std::string envArg = "-e " + key + "=" + value;
                envVariables.push_back(std::move(envArg));
                return *this;
            } */

            // Finalizes and constructs the full command string
            std::string build()
            {
                std::stringstream ss;
                ss << baseCommand;

                // 1. Add Default and Custom Arguments
                for (const auto& arg : args)
                {
                    ss << " " << arg;
                }

                /* TODO:
                2. Add Environment Variables(which are dynamically created strings)
                for (const auto& env : envVariables)
                {
                    ss << " " << env;
                } */

                return ss.str();
            }
        };

    private:
        WSLAVirtualMachine* m_pVM;
        std::map<int, ContainerInfo> m_containers;
        std::recursive_mutex m_containersLock;
        std::atomic_int m_containerId = 1;

        bool IsContainerRunning(const int containerId);

        // Start a new nerdctl process
        ContainerProcess StartProcess(const std::string& command, const ContainerProcessOptions& processOptions);
        ContainerProcessResult WaitForProcess(const ContainerProcess& process, uint64_t waitTimeOutMs = 60000);

        bool CheckPortConflicts(const std::vector<PortMapping>& portMappings);

        std::string prepareNerdctlRunCommand(std::string_view image, std::string_view containerName, const ContainerOptions& options);
    };
} // namespace wsl::windows::service::wsla
