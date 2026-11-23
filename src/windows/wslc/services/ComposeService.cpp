/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.cpp

Abstract:

    Implements minimal compose CLI operations.

--*/

#include "precomp.h"
#include "ComposeService.h"
#include "ConsoleService.h"
#include <array>
#include <filesystem.hpp>
#include <relay.hpp>
#include <WSLCContainerEntry.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

namespace {

    struct CapturedComposeDocuments
    {
        explicit CapturedComposeDocuments(const std::wstring& path)
        {
            std::error_code error;
            sourcePath = wsl::windows::common::filesystem::GetCanonicalPath(path, error);
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(error.value()), wsl::shared::Localization::MessageWslcComposePathResolveFailed(path), static_cast<bool>(error));
            projectDirectory = sourcePath.parent_path();

            // Capture document bytes and source context as the input snapshot for interpretation.
            std::ifstream stream{sourcePath, std::ios::binary};
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), wsl::shared::Localization::MessageWslcComposeFileOpenFailed(sourcePath.wstring()), !stream);

            std::array<char, 4096> buffer{};
            while (stream.read(buffer.data(), buffer.size()) || stream.gcount() > 0)
            {
                const auto bytesRead = gsl::narrow<size_t>(stream.gcount());
                THROW_HR_IF_MSG(
                    HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                    bytesRead > ULONG_MAX - content.size(),
                    "Compose document exceeds the maximum transport size: %ls",
                    sourcePath.c_str());
                content.append(buffer.data(), bytesRead);
            }

            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                wsl::shared::Localization::MessageWslcComposeFileReadFailed(sourcePath.wstring()),
                stream.bad() || !stream.eof());
            THROW_HR_IF(E_INVALIDARG, content.empty());

            document.SourcePath = sourcePath.c_str();
            document.BaseDirectory = projectDirectory.c_str();
            document.Content = reinterpret_cast<const byte*>(content.data());
            document.ContentSize = gsl::narrow<ULONG>(content.size());

            documents.ProjectDirectory = projectDirectory.c_str();
            documents.Documents = &document;
            documents.DocumentCount = 1;
        }

        std::filesystem::path sourcePath;
        std::filesystem::path projectDirectory;
        std::string content;
        WSLCComposeDocument document{};
        WSLCComposeDocumentCollection documents{};
    };

    struct ComposeOperationResult
    {
        NON_COPYABLE(ComposeOperationResult);

        ComposeOperationResult() = default;

        ComposeOperationResult(ComposeOperationResult&& other) noexcept : value(other.value)
        {
            other.value = {};
        }

        ~ComposeOperationResult()
        {
            CoTaskMemFree(value.ErrorMessage);
            CoTaskMemFree(value.AffectedContainers);
        }

        WSLCComposeOperationResult value{};
    };

    ComposeOperationResult Execute(
        models::Session& session,
        const ComposeProjectReference& project,
        WSLCComposeAction action,
        HANDLE cancelEvent,
        std::optional<ULONG> timeout = std::nullopt)
    {
        std::optional<CapturedComposeDocuments> captured;
        WSLCComposeOperationRequest request{};
        request.Action = action;
        if (const auto* path = std::get_if<std::filesystem::path>(&project))
        {
            captured.emplace(path->wstring());
            request.Project.Type = WSLCComposeProjectTypeDocuments;
            request.Project.Value.Documents = &captured->documents;
        }
        else
        {
            request.Project.Type = WSLCComposeProjectTypeProjectKey;
            request.Project.Value.ProjectKey = std::get<std::string>(project).c_str();
        }

        if (timeout.has_value())
        {
            request.ActionOptions.Type = WSLCComposeActionOptionsTypeStop;
            request.ActionOptions.Value.Stop.TimeoutSeconds = *timeout;
        }
        else
        {
            request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
        }

        ComposeOperationResult result;
        THROW_IF_FAILED(session.Get()->ExecuteComposeOperation(&request, nullptr, cancelEvent, &result.value));
        const bool failedWithMessage = FAILED(result.value.Result) && result.value.ErrorMessage != nullptr;
        THROW_HR_WITH_USER_ERROR_IF(result.value.Result, result.value.ErrorMessage, failedWithMessage);
        THROW_IF_FAILED(result.value.Result);
        THROW_HR_IF(E_UNEXPECTED, result.value.Status != WSLCComposeOperationStatusSucceeded);
        return result;
    }

    int AttachContainers(models::Session& session, const ComposeOperationResult& result)
    {
        [[maybe_unused]] auto operation = session.BeginContainerOperation();
        common::io::MultiHandleWait io;

        for (ULONG index = 0; index < result.value.AffectedContainersCount; ++index)
        {
            const auto& entry = result.value.AffectedContainers[index];
            wil::com_ptr<IWSLCContainer> container;
            THROW_IF_FAILED(session.Get()->OpenContainer(entry.Id, &container));

            wsl::windows::common::wslutil::COMOutputHandle stdinHandle;
            wsl::windows::common::wslutil::COMOutputHandle stdoutHandle;
            wsl::windows::common::wslutil::COMOutputHandle stderrHandle;

            THROW_IF_FAILED(container->Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle));

            io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
                stdoutHandle.Release(), GetStdHandle(STD_OUTPUT_HANDLE)));

            io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
                stderrHandle.Release(), GetStdHandle(STD_ERROR_HANDLE)));
        }

        io.Run({});
        return 0;
    }

} // namespace

std::wstring ComposeService::FormatProjectStatus(const WSLCComposeProjectSummary& project, models::FormatType format)
{
    std::wstring result;
    const auto append = [&](std::wstring_view name, ULONG count) {
        if (count == 0)
        {
            return;
        }

        if (!result.empty())
        {
            result += L", ";
        }

        result += std::format(L"{}({})", name, count);
    };

    switch (format)
    {
    case models::FormatType::Table:
        append(wsl::shared::Localization::WSLCCLI_ContainerStateCreated(), project.CreatedContainersCount);
        append(wsl::shared::Localization::WSLCCLI_ContainerStateExited(), project.ExitedContainersCount);
        append(wsl::shared::Localization::WSLCCLI_ComposeProjectStateOther(), project.OtherContainersCount);
        append(wsl::shared::Localization::WSLCCLI_ContainerStateRunning(), project.RunningContainersCount);
        break;
    case models::FormatType::Json:
        append(L"created", project.CreatedContainersCount);
        append(L"exited", project.ExitedContainersCount);
        append(L"other", project.OtherContainersCount);
        append(L"running", project.RunningContainersCount);
        break;
    default:
        THROW_HR(E_UNEXPECTED);
    }

    return result;
}

std::vector<models::ComposeProjectInformation> ComposeService::List(models::Session& session, bool all, models::FormatType format)
{
    WSLCComposeProjectListOptions options{};
    options.All = all;

    wil::unique_cotaskmem_array_ptr<WSLCComposeProjectSummary> projects;
    THROW_IF_FAILED(session.Get()->ListComposeProjects(&options, &projects, projects.size_address<ULONG>()));

    std::vector<models::ComposeProjectInformation> result;
    result.reserve(projects.size());
    for (const auto& project : projects)
    {
        result.emplace_back(project.ProjectKey, wsl::shared::string::WideToMultiByte(FormatProjectStatus(project, format)));
    }

    return result;
}

void ComposeService::Create(Terminal&, models::Session& session, const std::wstring& path, HANDLE cancelEvent)
{
    Execute(session, ComposeProjectReference{std::filesystem::path{path}}, WSLCComposeActionCreate, cancelEvent);
}

int ComposeService::Up(Terminal&, models::Session& session, const std::wstring& path, HANDLE cancelEvent)
{
    const auto result = Execute(session, ComposeProjectReference{std::filesystem::path{path}}, WSLCComposeActionUp, cancelEvent);
    return AttachContainers(session, result);
}

void ComposeService::Start(Terminal&, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    Execute(session, project, WSLCComposeActionStart, cancelEvent);
}

int ComposeService::Attach(Terminal&, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    auto result = Execute(session, project, WSLCComposeActionAttach, cancelEvent);
    return AttachContainers(session, result);
}

void ComposeService::Stop(Terminal&, models::Session& session, const ComposeProjectReference& project, ULONG timeout, HANDLE cancelEvent)
{
    Execute(session, project, WSLCComposeActionStop, cancelEvent, timeout);
}

void ComposeService::Remove(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    const auto result = Execute(session, project, WSLCComposeActionRemove, cancelEvent);
    if (result.value.AffectedContainersCount == 0)
    {
        terminal.Info(L"{}\n", wsl::shared::Localization::WSLCCLI_ComposeNoStoppedContainers());
    }
}

} // namespace wsl::windows::wslc::services
