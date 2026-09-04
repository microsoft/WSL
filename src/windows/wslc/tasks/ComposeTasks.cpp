// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ArgumentConvertedTypes.h"
#include "ComposeService.h"
#include "ComposeTasks.h"
#include "TableOutput.h"

using namespace wsl::windows::wslc::argument;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::shared;

namespace wsl::windows::wslc::task {

namespace {

    std::wstring ComposePath(CLIExecutionContext& Context)
    {
        return std::filesystem::absolute(Context.Args.GetValue<ArgType::Path>()).wstring();
    }

} // namespace

void AttachCompose(CLIExecutionContext& Context)
{
    Context.ExitCode =
        ComposeService::Attach(Context.Terminal, Context.Data.Get<Data::Session>(), ComposePath(Context), Context.CreateCancelEvent());
}

void CreateCompose(CLIExecutionContext& Context)
{
    ComposeService::Create(Context.Terminal, Context.Data.Get<Data::Session>(), ComposePath(Context), Context.CreateCancelEvent());
}

void GetComposeProjects(CLIExecutionContext& Context)
{
    Context.Data.Add<Data::ComposeProjects>(ComposeService::List(Context.Data.Get<Data::Session>(), Context.Args.GetValue<ArgType::All>()));
}

void ListComposeProjects(CLIExecutionContext& Context)
{
    const auto& projects = Context.Data.Get<Data::ComposeProjects>();
    if (Context.Args.GetValue<ArgType::Quiet>())
    {
        for (const auto& project : projects)
        {
            Context.Terminal.Output(L"{}\n", wsl::shared::string::MultiByteToWide(project.Name));
        }

        return;
    }

    switch (Context.Args.GetValue<ArgType::Format>(models::FormatType::Table))
    {
    case models::FormatType::Json:
        for (const auto& project : projects)
        {
            Context.Terminal.Output(L"{}\n", wsl::shared::ToJsonW(project, wsl::shared::c_jsonCompactIndent));
        }
        break;

    case models::FormatType::Table:
    {
        TableOutput<2> table(
            Context.Terminal,
            TableOutput<2>::header_t{Localization::WSLCCLI_TableHeaderName(), Localization::WSLCCLI_TableHeaderStatus()},
            projects.size());
        for (const auto& project : projects)
        {
            table.WriteRow({wsl::shared::string::MultiByteToWide(project.Name), wsl::shared::string::MultiByteToWide(project.Status)});
        }

        table.Complete();
        break;
    }

    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void StartCompose(CLIExecutionContext& Context)
{
    ComposeService::Start(Context.Terminal, Context.Data.Get<Data::Session>(), ComposePath(Context), Context.CreateCancelEvent());
}

void StopCompose(CLIExecutionContext& Context)
{
    constexpr LONG c_defaultTimeout = 10;
    const LONG timeout = Context.Args.Contains(ArgType::Time) ? Context.Args.GetValue<ArgType::Time>() : c_defaultTimeout;
    THROW_HR_IF(E_INVALIDARG, timeout < 0);

    ComposeService::Stop(
        Context.Terminal, Context.Data.Get<Data::Session>(), ComposePath(Context), static_cast<ULONG>(timeout), Context.CreateCancelEvent());
}

void UpCompose(CLIExecutionContext& Context)
{
    Context.ExitCode =
        ComposeService::Up(Context.Terminal, Context.Data.Get<Data::Session>(), ComposePath(Context), Context.CreateCancelEvent());
}

} // namespace wsl::windows::wslc::task
