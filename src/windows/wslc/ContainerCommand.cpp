/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCommand.cpp

Abstract:

    This file contains the ContainerCommand implementation

--*/
#include "precomp.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
#include "TableOutput.h"
#include "Utils.h"
#include <CommandLine.h>
#include <format>

namespace wslc::commands {

using namespace wsl::shared;
using namespace wslc::models;
using namespace wslc::services;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::docker_schema::InspectContainer;

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
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_image, L"Error: image name is required.");
    auto session = m_sessionService.CreateSession();
    m_options.Arguments = Arguments();
    ContainerService containerService;
    return containerService.Run(session, m_image, m_options);
}

int ContainerCreateCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_image, L"Error: image name is required.");
    auto session = m_sessionService.CreateSession();
    m_options.Arguments = Arguments();
    m_options.Name = GetContainerName(m_options.Name);
    ContainerService containerService;
    auto result = containerService.Create(session, m_image, m_options);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(result.Id));
    return 0;
}

int ContainerStartCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_id, L"Error: container value is required.");
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;
    containerService.Start(session, m_id);
    return 0;
}

int ContainerStopCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto containersToStop = Arguments();
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;

    if(m_all)
    {
        containersToStop.clear();
        auto allContainers = containerService.List(session);
        for(const auto& container : allContainers)
        {
            containersToStop.push_back(container.Name);
        }
    }

    for (const auto& id : containersToStop)
    {
        containerService.Stop(session, id, m_options);
    }
    return 0;
}

int ContainerKillCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto containersToKill = Arguments();
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;

    if(m_all)
    {
        containersToKill.clear();
        auto allContainers = containerService.List(session);
        for(const auto& container : allContainers)
        {
            containersToKill.push_back(container.Name);
        }
    }

    for (const auto& id : containersToKill)
    {
        containerService.Kill(session, id, m_options.Signal);
    }
    return 0;
}

int ContainerDeleteCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto containersToDelete = Arguments();
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;

    if(m_all)
    {
        containersToDelete.clear();
        auto allContainers = containerService.List(session);
        for(const auto& container : allContainers)
        {
            containersToDelete.push_back(container.Name);
        }
    }

    for (const auto& id : containersToDelete)
    {
        containerService.Delete(session, id, m_force);
    }
    return 0;
}

int ContainerListCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;
    auto containers = containerService.List(session);
    auto argIds = Arguments();

    // Filter by running state if --all is not specified
    if (!m_all)
    {
        auto shouldRemove = [](const wslc::models::ContainerInformation& container) {
            return container.State != WSLA_CONTAINER_STATE::WslaContainerStateRunning;
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    // Filter by name if provided
    if (!argIds.empty())
    {
        auto shouldRemove = [&argIds](const wslc::models::ContainerInformation& container) {
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
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_id, L"Error: container value is required.");
    auto arguments = Arguments();
    CMD_ARG_ARRAY_REQUIRED(arguments, L"Error: at least one command needs to be specified.");
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;
    wslc::models::ExecContainerOptions options;
    options.Arguments = arguments;
    options.Interactive = m_options.Interactive;
    options.TTY = m_options.TTY;
    return containerService.Exec(session, m_id, options);
}

int ContainerInspectCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto arguments = Arguments();
    CMD_ARG_ARRAY_REQUIRED(arguments, L"Error: at least one command needs to be specified.");
    auto session = m_sessionService.CreateSession();
    wslc::services::ContainerService containerService;
    std::vector<InspectContainer> result;

    for (const auto& id : arguments)
    {
        auto inspectData = containerService.Inspect(session, id);
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

    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_subverb, L"Error: Invalid or missing subcommand.");
    return 0;
}
}
