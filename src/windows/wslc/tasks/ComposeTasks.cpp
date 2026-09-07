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

    bool TryResolveComposePath(const std::filesystem::path& value, std::filesystem::path& result)
    {
        std::error_code error;
        const bool isFile = std::filesystem::is_regular_file(value, error);
        if (error)
        {
            if (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND)
            {
                return false;
            }

            const auto errorResult = HRESULT_FROM_WIN32(error.value());
            THROW_HR_WITH_USER_ERROR(
                errorResult,
                Localization::MessageWslcFailedToOpenFile(value.wstring(), wsl::windows::common::wslutil::GetSystemErrorString(errorResult)));
        }
        if (!isFile)
        {
            return false;
        }

        result = std::filesystem::absolute(value, error);
        if (error)
        {
            const auto errorResult = HRESULT_FROM_WIN32(error.value());
            THROW_HR_WITH_USER_ERROR(
                errorResult,
                Localization::MessageWslcFailedToOpenFile(value.wstring(), wsl::windows::common::wslutil::GetSystemErrorString(errorResult)));
        }

        return true;
    }

    std::wstring ComposePath(CLIExecutionContext& context)
    {
        const std::filesystem::path value{context.Args.GetValue<ArgType::Path>()};
        std::filesystem::path result;
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            Localization::MessageWslcComposeProjectPathNotFound(value.wstring()),
            !TryResolveComposePath(value, result));
        return result.wstring();
    }

    ComposeProjectReference ComposeProject(CLIExecutionContext& context, models::Session& session)
    {
        const std::filesystem::path value{context.Args.GetValue<ArgType::Project>()};
        std::filesystem::path path;
        if (TryResolveComposePath(value, path))
        {
            return ComposeProjectReference{std::move(path)};
        }

        const auto projectKey =
            wsl::shared::string::WideToMultiByte(wsl::shared::string::AsciiToLower(std::wstring_view{value.native()}));
        const auto projects = ComposeService::List(session, true);
        if (std::ranges::find(projects, projectKey, &models::ComposeProjectInformation::Name) != projects.end())
        {
            return ComposeProjectReference{projectKey};
        }

        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Localization::MessageWslcComposeProjectOrFileNotFound(value.wstring()));
    }

} // namespace

void AttachCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode = ComposeService::Attach(context.Terminal, session, ComposeProject(context, session), context.CreateCancelEvent());
}

void CreateCompose(CLIExecutionContext& context)
{
    ComposeService::Create(context.Terminal, context.Data.Get<Data::Session>(), ComposePath(context), context.CreateCancelEvent());
}

void GetComposeProjects(CLIExecutionContext& context)
{
    context.Data.Add<Data::ComposeProjects>(ComposeService::List(context.Data.Get<Data::Session>(), context.Args.GetValue<ArgType::All>()));
}

void ListComposeProjects(CLIExecutionContext& context)
{
    const auto& projects = context.Data.Get<Data::ComposeProjects>();
    if (context.Args.GetValue<ArgType::Quiet>())
    {
        for (const auto& project : projects)
        {
            context.Terminal.Output(L"{}\n", wsl::shared::string::MultiByteToWide(project.Name));
        }

        return;
    }

    switch (context.Args.GetValue<ArgType::Format>(models::FormatType::Table))
    {
    case models::FormatType::Json:
        for (const auto& project : projects)
        {
            context.Terminal.Output(L"{}\n", wsl::shared::ToJsonW(project, wsl::shared::c_jsonCompactIndent));
        }
        break;

    case models::FormatType::Table:
    {
        TableOutput<2> table(
            context.Terminal,
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

void RemoveCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Remove(context.Terminal, session, ComposeProject(context, session), context.CreateCancelEvent());
}

void StartCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Start(context.Terminal, session, ComposeProject(context, session), context.CreateCancelEvent());
}

void StopCompose(CLIExecutionContext& context)
{
    constexpr LONG c_defaultTimeout = 10;
    const LONG timeout = context.Args.Contains(ArgType::Time) ? context.Args.GetValue<ArgType::Time>() : c_defaultTimeout;
    THROW_HR_IF(E_INVALIDARG, timeout < 0);

    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Stop(context.Terminal, session, ComposeProject(context, session), static_cast<ULONG>(timeout), context.CreateCancelEvent());
}

void UpCompose(CLIExecutionContext& context)
{
    const auto cancelEvent = context.CreateCancelEvent();
    const auto forceCancelEvent = context.CreateForceCancelEvent();
    context.ExitCode =
        ComposeService::Up(context.Terminal, context.Data.Get<Data::Session>(), ComposePath(context), cancelEvent, forceCancelEvent);
}

} // namespace wsl::windows::wslc::task
