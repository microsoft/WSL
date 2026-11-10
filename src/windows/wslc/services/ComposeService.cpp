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
#include <relay.hpp>
#include <WSLCContainerEntry.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

namespace {

    std::string FormatProjectStatus(const WSLCComposeProjectSummary& project)
    {
        std::string result;
        const auto append = [&](std::string_view name, ULONG count) {
            if (count == 0)
            {
                return;
            }

            if (!result.empty())
            {
                result += ", ";
            }

            result += std::format("{}({})", name, count);
        };

        append("created", project.CreatedContainersCount);
        append("exited", project.ExitedContainersCount);
        append("other", project.OtherContainersCount);
        append("running", project.RunningContainersCount);
        return result;
    }

    struct CapturedComposeDocuments
    {
        explicit CapturedComposeDocuments(const std::wstring& path)
        {
            std::error_code error;
            sourcePath = std::filesystem::absolute(path, error).lexically_normal();
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(error.value()), wsl::shared::Localization::MessageWslcComposePathResolveFailed(path), static_cast<bool>(error));
            projectDirectory = sourcePath.parent_path();

            // Capture document bytes and source context as the input snapshot for interpretation.
            std::ifstream stream{sourcePath, std::ios::binary | std::ios::ate};
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), wsl::shared::Localization::MessageWslcComposeFileOpenFailed(sourcePath.wstring()), !stream);

            const auto documentSize = stream.tellg();
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                wsl::shared::Localization::MessageWslcComposeFileReadFailed(sourcePath.wstring()),
                documentSize < 0);
            THROW_HR_IF(E_INVALIDARG, documentSize == 0);
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                static_cast<uint64_t>(documentSize) > ULONG_MAX,
                "Compose document exceeds the maximum transport size: %ls",
                sourcePath.c_str());
            const auto contentSize = gsl::narrow<ULONG>(static_cast<uint64_t>(documentSize));
            const auto streamSize = gsl::narrow<std::streamsize>(contentSize);

            content.resize(contentSize);
            stream.seekg(0, std::ios::beg);
            stream.read(content.data(), streamSize);
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                wsl::shared::Localization::MessageWslcComposeFileReadFailed(sourcePath.wstring()),
                stream.gcount() != streamSize);

            char extraByte{};
            stream.read(&extraByte, 1);
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                wsl::shared::Localization::MessageWslcComposeFileReadFailed(sourcePath.wstring()),
                stream.gcount() != 0);

            document.SourcePath = sourcePath.c_str();
            document.BaseDirectory = projectDirectory.c_str();
            document.Content = reinterpret_cast<const byte*>(content.data());
            document.ContentSize = contentSize;

            documents.ProjectDirectory = projectDirectory.c_str();
            documents.Documents = &document;
            documents.DocumentCount = 1;
        }

        std::filesystem::path sourcePath;
        std::filesystem::path projectDirectory;
        std::vector<char> content;
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
        if (const auto* path = std::get_if<std::filesystem::path>(&project.Value))
        {
            captured.emplace(path->wstring());
            request.Project.Type = WSLCComposeProjectTypeDocuments;
            request.Project.Value.Documents = &captured->documents;
        }
        else
        {
            request.Project.Type = WSLCComposeProjectTypeProjectKey;
            request.Project.Value.ProjectKey = std::get<std::string>(project.Value).c_str();
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
        THROW_HR_WITH_USER_ERROR_IF(
            result.value.Result, result.value.ErrorMessage, FAILED(result.value.Result) && result.value.ErrorMessage != nullptr);
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

std::vector<models::ComposeProjectInformation> ComposeService::List(models::Session& session, bool all)
{
    WSLCComposeProjectListOptions options{};
    options.All = all;

    wil::unique_cotaskmem_array_ptr<WSLCComposeProjectSummary> projects;
    THROW_IF_FAILED(session.Get()->ListComposeProjects(&options, &projects, projects.size_address<ULONG>()));

    std::vector<models::ComposeProjectInformation> result;
    result.reserve(projects.size());
    for (const auto& project : projects)
    {
        result.emplace_back(project.ProjectKey, FormatProjectStatus(project));
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
