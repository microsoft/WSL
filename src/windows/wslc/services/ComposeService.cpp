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
#include <relay.hpp>
#include <WSLCContainerEntry.h>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

namespace {

    std::string ReadComposeDocument(const std::filesystem::path& sourcePath)
    {
        std::ifstream stream{sourcePath, std::ios::binary};
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), wsl::shared::Localization::MessageWslcComposeFileOpenFailed(sourcePath.wstring()), !stream);

        std::string content;
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

        return content;
    }

    struct CapturedComposeDocuments
    {
        NON_COPYABLE(CapturedComposeDocuments);
        NON_MOVABLE(CapturedComposeDocuments);

        explicit CapturedComposeDocuments(const models::ComposeProjectFiles& project) :
            sourcePaths(project.Files), projectDirectory(project.ProjectDirectory), projectName(project.ProjectName)
        {
            THROW_HR_IF(E_INVALIDARG, sourcePaths.empty());

            // Capture document bytes and source context as the input snapshot for interpretation.
            contents.reserve(sourcePaths.size());
            for (const auto& sourcePath : sourcePaths)
            {
                contents.push_back(ReadComposeDocument(sourcePath));
            }

            // The document entries point into sourcePaths and contents, which are not modified after this point.
            documentEntries.resize(sourcePaths.size());
            for (size_t index = 0; index < sourcePaths.size(); ++index)
            {
                auto& document = documentEntries[index];
                document.SourcePath = sourcePaths[index].c_str();
                document.BaseDirectory = projectDirectory.c_str();
                document.Content = reinterpret_cast<const byte*>(contents[index].data());
                document.ContentSize = gsl::narrow<ULONG>(contents[index].size());
            }

            documents.ProjectDirectory = projectDirectory.c_str();
            documents.ExplicitProjectName = projectName.has_value() ? projectName->c_str() : nullptr;
            documents.Documents = documentEntries.data();
            documents.DocumentCount = gsl::narrow<ULONG>(documentEntries.size());
        }

        std::vector<std::filesystem::path> sourcePaths;
        std::filesystem::path projectDirectory;
        std::optional<std::string> projectName;
        std::vector<std::string> contents;
        std::vector<WSLCComposeDocument> documentEntries;
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
        models::Session& session, const models::ComposeProject& project, WSLCComposeAction action, HANDLE cancelEvent, std::optional<ULONG> timeout = std::nullopt)
    {
        std::optional<CapturedComposeDocuments> captured;
        WSLCComposeOperationRequest request{};
        request.Action = action;
        if (const auto* files = std::get_if<models::ComposeProjectFiles>(&project.Value))
        {
            captured.emplace(*files);
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

void ComposeService::Create(Terminal&, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent)
{
    Execute(session, project, WSLCComposeActionCreate, cancelEvent);
}

int ComposeService::Up(Terminal&, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent)
{
    const auto result = Execute(session, project, WSLCComposeActionUp, cancelEvent);
    return AttachContainers(session, result);
}

void ComposeService::Start(Terminal&, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent)
{
    Execute(session, project, WSLCComposeActionStart, cancelEvent);
}

int ComposeService::Attach(Terminal&, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent)
{
    auto result = Execute(session, project, WSLCComposeActionAttach, cancelEvent);
    return AttachContainers(session, result);
}

void ComposeService::Stop(Terminal&, models::Session& session, const models::ComposeProject& project, ULONG timeout, HANDLE cancelEvent)
{
    Execute(session, project, WSLCComposeActionStop, cancelEvent, timeout);
}

} // namespace wsl::windows::wslc::services
