#pragma once
#include "ICommand.h"
#include "ContainerService.h"

namespace wslc::commands
{
// wslc container run
class ContainerRunCommand : public ICommand
{
public:
    std::string Name() const override { return "run"; }
    std::string Description() const override { return "Create and run a new container from an image."; }
    std::vector<std::string> Options() const override
    {
        return {
            "image (pos. 0): Image name",
            "arguments (pos. 1..): Arguments to pass to container's init process",
            "-t, --tty: Open a TTY with the container process",
            "-i, --interactive: Keep stdin open",
            "-d, --detach: Run container in background",
            "--name <name>: Assign a name to the container that will be used as its container id",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_image}, 0);
        parser.AddArgument(m_options.Interactive, L"--interactive", 'i');
        parser.AddArgument(m_options.TTY, L"--tty", 't');
        parser.AddArgument(m_options.Detach, L"--detach", 'd');
        parser.AddArgument(wsl::shared::Utf8String{m_options.Name}, L"--name");
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    wslc::models::ContainerRunOptions m_options;
    std::string m_image;
};

// wslc container create
class ContainerCreateCommand : public ICommand
{
public:
    std::string Name() const override { return "create"; }
    std::string Description() const override { return "Creates a container but does not start it."; }
    std::vector<std::string> Options() const override
    {
        return {
            "image (pos. 0): Image name",
            "arguments (pos. 1..): Arguments to pass to container's init process",
            "-t, --tty: Open a TTY with the container process",
            "-i, --interactive: Keep stdin open",
            "--name <name>: Assign a name to the container that will be used as its container id",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_image}, 0);
        parser.AddArgument(m_options.Interactive, L"--interactive", 'i');
        parser.AddArgument(m_options.TTY, L"--tty", 't');
        parser.AddArgument(wsl::shared::Utf8String{m_options.Name}, L"--name");
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    wslc::models::ContainerCreateOptions m_options;
    std::string m_image;
};

// wslc container start
class ContainerStartCommand : public ICommand
{
public:
    std::string Name() const override { return "start"; }
    std::string Description() const override { return "Start a container."; }
    std::vector<std::string> Options() const override
    {
        return {
            "id (pos. 0): Container ID",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_id}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_id;
};

// wslc container stop
class ContainerStopCommand : public ICommand
{
public:
    std::string Name() const override { return "stop"; }
    std::string Description() const override { return "Stop a container."; }
    std::vector<std::string> Options() const override
    {
        return {
            "ids (pos. 0..): Container IDs",
            "-a, --all: Stop all the running containers",
            "-s, --signal <signal>: Signal to send to the specified containers (default: SIGTERM)",
            "-t, --time <time>: Time in seconds to wait before killing the containers (default: 5)",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(m_all, L"--all", 'a');
        parser.AddArgument(wsl::shared::Integer{m_options.Signal}, L"--signal", 's');
        parser.AddArgument(wsl::shared::Integer{m_options.Timeout}, L"--time", 't');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    wslc::models::StopContainerOptions m_options;
    bool m_all;
};

// wslc container kill
class ContainerKillCommand : public ICommand
{
public:
    std::string Name() const override { return "kill"; }
    std::string Description() const override { return "Sends SIGKILL (default option) to running containers to immediately kill the containers."; }
    std::vector<std::string> Options() const override
    {
        return {
            "ids (pos. 0..): Container IDs",
            "-a, --all: Stop all the running containers",
            "-s, --signal <signal>: Signal to send to the container(s) (default: SIGKILL)",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(m_all, L"--all", 'a');
        parser.AddArgument(wsl::shared::Integer{m_options.Signal}, L"--signal", 's');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    wslc::models::KillContainerOptions m_options;
    bool m_all;
};


// wslc container delete
class ContainerDeleteCommand : public ICommand
{
public:
    std::string Name() const override { return "delete"; }
    std::string Description() const override { return "Deletes specified container(s)."; }
    std::vector<std::string> Options() const override
    {
        return {
            "ids (pos. 0..): Container IDs",
            "-a, --all: Stop all the running containers",
            "-f, --force: Delete containers even if they are running ",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(m_all, L"--all", 'a');
        parser.AddArgument(m_force, L"--force", 'f');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    bool m_force;
    bool m_all;
};


// wslc container list
class ContainerListCommand : public ICommand
{
public:
    std::string Name() const override { return "list"; }
    std::string Description() const override { return "List running containers."; }
    std::vector<std::string> Options() const override
    {
        return {
            "ids (pos. 0..): Container IDs",
            "-a, --all: List containers that are not running",
            "--format: Output formatting (json or table. Default: table)",
            "-q, --quiet: Outputs the container IDs only",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(m_all, L"--all", 'a');
        parser.AddArgument(wsl::shared::Utf8String{m_format}, L"--format");
        parser.AddArgument(m_quiet, L"--quiet", 'q');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    bool m_all;
    std::string m_format;
    bool m_quiet;
};


// wslc container exec
class ContainerExecCommand : public ICommand
{
public:
    std::string Name() const override { return "exec"; }
    std::string Description() const override { return "Allows execution of a command inside of a running container."; }
    std::vector<std::string> Options() const override
    {
        return {
            "id (pos. 0): Container ID",
            "arguments (pos. 1..): Arguments to pass to the intended process/command to be run inside the container",
            "-t, --tty: Open a TTY with the container process",
            "-i, --interactive: Keep stdin open",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_id}, 0);
        parser.AddArgument(m_options.Interactive, L"--interactive", 'i');
        parser.AddArgument(m_options.TTY, L"--tty", 't');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_id;
    wslc::models::ExecContainerOptions m_options;
};

// wslc container inspect
class ContainerInspectCommand : public ICommand
{
public:
    std::string Name() const override { return "inspect"; }
    std::string Description() const override { return "Outputs details about the container(s) specified using container ID(s) in JSON format."; }
    std::vector<std::string> Options() const override
    {
        return {
            "ids (pos. 0..): Container IDs",
        };
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;
};

// wslc container
class ContainerCommand : public ICommand
{
public:
    std::string Name() const override { return "container"; }
    std::string Description() const override { return "Manage containers."; }
    std::vector<std::string> Options() const override
    {
        return {
            m_run.GetShortDescription(),
            m_create.GetShortDescription(),
            m_start.GetShortDescription(),
            m_stop.GetShortDescription(),
            m_kill.GetShortDescription(),
            m_delete.GetShortDescription(),
            m_list.GetShortDescription(),
            m_exec.GetShortDescription(),
            m_inspect.GetShortDescription()
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_subverb}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_subverb;
    ContainerRunCommand m_run;
    ContainerCreateCommand m_create;
    ContainerStartCommand m_start;
    ContainerStopCommand m_stop;
    ContainerKillCommand m_kill;
    ContainerDeleteCommand m_delete;
    ContainerListCommand m_list;
    ContainerExecCommand m_exec;
    ContainerInspectCommand m_inspect;
};
}
