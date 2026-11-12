// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "CLIExecutionContext.h"
#include "ComposeModel.h"
#include "Task.h"

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {

// How a subcommand identifies its project when a project name is supplied.
enum class ComposeProjectSelection
{
    // Always load the project from its compose files.
    Files,

    // Use the project name when no compose files are supplied.
    NameWithoutFiles,

    // Use the project name whenever it is supplied.
    NamePreferred,
};

wsl::windows::wslc::models::ComposeProject ResolveComposeProjectFromArgs(
    wsl::windows::wslc::argument::ArgMap& args, ComposeProjectSelection selection, const std::filesystem::path& currentDirectory);

struct ResolveComposeProject : public Task
{
    ResolveComposeProject(ComposeProjectSelection selection) : m_selection(selection)
    {
    }

    void operator()(CLIExecutionContext& context) const override;

private:
    ComposeProjectSelection m_selection;
};

void AttachCompose(CLIExecutionContext& context);
void CreateCompose(CLIExecutionContext& context);
void StartCompose(CLIExecutionContext& context);
void StopCompose(CLIExecutionContext& context);
void UpCompose(CLIExecutionContext& context);

} // namespace wsl::windows::wslc::task
