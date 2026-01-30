#include "precomp.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
#include "TableOutput.h"
#include "Utils.h"
#include <CommandLine.h>
#include <format>

namespace wslc::commands {

using wslc::services::ContainerService;
using wslc::services::CreateOptions;
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::docker_schema::InspectContainer;

#define IF_HELP_PRINT_HELP() if (m_help) { PrintHelp(); return 0; }
#define ARG_REQUIRED(arg, msg) if (arg.empty()) { wslutil::PrintMessage(msg, stderr); PrintHelp(); return E_INVALIDARG; }

static std::string GetContainerName(const std::string& name)
{
    if (!name.empty())
    {
        return name;
    }

    GUID guid;
    THROW_IF_FAILED(CoCreateGuid(&guid));
    return wsl::shared::string::GuidToString<char>(guid, wsl::shared::string::GuidToStringFlags::None);
}

static std::wstring ContainerStateToString(WSLA_CONTAINER_STATE state)
{
    switch (state)
    {
    case WSLA_CONTAINER_STATE::WslaContainerStateCreated:
        return L"created";
    case WSLA_CONTAINER_STATE::WslaContainerStateRunning:
        return L"running";
    case WSLA_CONTAINER_STATE::WslaContainerStateDeleted:
        return L"stopped";
    case WSLA_CONTAINER_STATE::WslaContainerStateExited:
        return L"exited";
    case WSLA_CONTAINER_STATE::WslaContainerStateInvalid:
    default:
        return L"invalid";
    }
}

int ContainerRunCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    IF_HELP_PRINT_HELP();
    ARG_REQUIRED(m_image, L"Error: image name is required.");
    auto session = OpenCLISession();
    CreateOptions options;
    options.TTY = m_tty;
    options.Interactive = m_interactive;
    options.Arguments = Arguments();
    options.Name = GetContainerName(m_name);
    ContainerService containerService;
    return containerService.Run(*session, m_image, options);
}

int ContainerCreateCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    THROW_HR_IF(E_INVALIDARG, m_image.empty());
    auto session = OpenCLISession();
    CreateOptions options;
    options.TTY = m_tty;
    options.Interactive = m_interactive;
    options.Arguments = Arguments();
    options.Name = GetContainerName(m_name);
    ContainerService containerService;
    auto result = containerService.Create(*session, m_image, options);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(result.Id));
    return 0;
}

int ContainerStartCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    THROW_HR_IF(E_INVALIDARG, m_id.empty());
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    containerService.Start(*session, m_id);
    return 0;
}

int ContainerStopCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    wslc::services::StopContainerOptions options;
    options.Signal = m_signal;
    options.Timeout = m_timeout;
    for (const auto& id : Arguments())
    {
        containerService.Stop(*session, id, options);
    }
    return 0;
}

int ContainerKillCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    for (const auto& id : Arguments())
    {
        containerService.Kill(*session, id, m_signal);
    }
    return 0;
}

int ContainerDeleteCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    for (const auto& id : Arguments())
    {
        containerService.Delete(*session, id, m_force);
    }
    return 0;
}

int ContainerListCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    auto containers = containerService.List(*session);
    auto argIds = Arguments();

    // Filter by running state if --all is not specified
    if (!m_all)
    {
        auto shouldRemove = [](const wslc::services::ContainerInformation& container) {
            return container.State != WSLA_CONTAINER_STATE::WslaContainerStateRunning;
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    // Filter by name if provided
    if (!argIds.empty())
    {
        auto shouldRemove = [&argIds](const wslc::services::ContainerInformation& container) {
            return std::find(argIds.begin(), argIds.end(), container.Name) == argIds.end();
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    if (m_quiet)
    {
        // Print only the container ids
        for (const auto& container : containers)
        {
            wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(container.Id));
        }
    }
    else if (m_format == "json")
    {
        auto json = wsl::shared::ToJson(containers);
        wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(json));
    }
    else
    {
        TablePrinter tablePrinter({L"ID", L"NAME", L"IMAGE", L"STATE"});
        for (const auto& container : containers)
        {
            tablePrinter.AddRow({
                std::wstring(container.Id.begin(), container.Id.end()),
                std::wstring(container.Name.begin(), container.Name.end()),
                std::wstring(container.Image.begin(), container.Image.end()),
                ContainerStateToString(container.State),
            });
        }

        tablePrinter.Print();
    }

    return 0;
}

int ContainerExecCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    THROW_HR_IF(E_INVALIDARG, m_id.empty());
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    containerService.Exec(*session, m_id, Arguments());
    return 0;
}

int ContainerInspectCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    auto session = OpenCLISession();
    wslc::services::ContainerService containerService;
    std::vector<InspectContainer> result;

    for (const auto& id : Arguments())
    {
        auto inspectData = containerService.Inspect(*session, id);
        result.push_back(inspectData);
    }

    auto json = wsl::shared::ToJson(result);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(json));
    return 0;
}

int ContainerCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    if (m_subverb == m_run.Name())
    {
        return m_run.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_create.Name())
    {
        return m_create.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_start.Name())
    {
        return m_start.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_stop.Name())
    {
        return m_stop.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_kill.Name())
    {
        return m_kill.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_delete.Name())
    {
        return m_delete.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_list.Name())
    {
        return m_list.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_exec.Name())
    {
        return m_exec.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_inspect.Name())
    {
        return m_inspect.Execute(commandLine, parserOffset + 1);
    }

    IF_HELP_PRINT_HELP();
    ARG_REQUIRED(m_subverb, L"Error: Invalid or missing subcommand.");
    return 0;
}

std::string ICommand::GetFullDescription() const
{
    std::stringstream ss;
    ss << Description() << std::endl;
    for (const auto& option : Options())
    {
        ss << "  " << option << std::endl;
    }
    ss << "  -h, --help: Print this help message" << std::endl;
    return ss.str();
}

std::string ICommand::GetShortDescription() const
{
    return std::format("{}: {}", Name(), Description());
}

void ICommand::PrintHelp() const
{
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(GetFullDescription()));
}
}
