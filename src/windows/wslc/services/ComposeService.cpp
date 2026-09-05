/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.cpp

Abstract:

    Implements minimal compose CLI operations.

--*/

#include "precomp.h"
#include "ComposeProgressCallback.h"
#include "ComposeService.h"
#include "ConsoleService.h"
#include <relay.hpp>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

namespace {

    std::string FormatProjectStatus(const WSLCComposeProjectSummary& Project)
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

        append("created", Project.CreatedContainersCount);
        append("exited", Project.ExitedContainersCount);
        append("other", Project.OtherContainersCount);
        append("running", Project.RunningContainersCount);
        return result;
    }

    struct CapturedComposeDocuments
    {
        explicit CapturedComposeDocuments(const std::wstring& Path)
        {
            sourcePath = std::filesystem::absolute(Path).lexically_normal();
            baseDirectory = sourcePath.parent_path();
            projectDirectory = baseDirectory;
            workingDirectory = std::filesystem::current_path();

            std::ifstream stream{sourcePath, std::ios::binary};
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream, "Failed to open compose document %ls", sourcePath.c_str());
            content.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
            THROW_HR_IF(E_INVALIDARG, content.empty() || content.size() > ULONG_MAX);

            document.SourcePath = sourcePath.c_str();
            document.BaseDirectory = baseDirectory.c_str();
            document.Content = reinterpret_cast<const byte*>(content.data());
            document.ContentSize = static_cast<ULONG>(content.size());

            documents.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
            documents.WorkingDirectory = workingDirectory.c_str();
            documents.ProjectDirectory = projectDirectory.c_str();
            documents.Documents = &document;
            documents.DocumentCount = 1;
        }

        std::filesystem::path sourcePath;
        std::filesystem::path baseDirectory;
        std::filesystem::path projectDirectory;
        std::filesystem::path workingDirectory;
        std::vector<char> content;
        WSLCComposeDocument document{};
        WSLCComposeDocuments documents{};
    };

    struct ComposeOperationResult
    {
        NON_COPYABLE(ComposeOperationResult);

        ComposeOperationResult() = default;

        ComposeOperationResult(ComposeOperationResult&& Other) noexcept : value(Other.value)
        {
            Other.value.AffectedContainers = nullptr;
            Other.value.AffectedContainersCount = 0;
        }

        ~ComposeOperationResult()
        {
            CoTaskMemFree(value.AffectedContainers);
        }

        WSLCComposeOperationResult value{};
    };

    ComposeOperationResult Execute(
        Terminal& Terminal,
        models::Session& Session,
        const std::wstring& Path,
        WSLCComposeAction Action,
        HANDLE CancelEvent,
        std::optional<ULONG> Timeout = std::nullopt)
    {
        CapturedComposeDocuments captured{Path};
        WSLCComposeOperationRequest request{};
        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        request.Action = Action;
        request.Project.Type = WSLCComposeProjectTypeDocuments;
        request.Project.Value.Documents = &captured.documents;
        if (Timeout.has_value())
        {
            request.ActionOptions.Type = WSLCComposeActionOptionsTypeStop;
            request.ActionOptions.Value.Stop.TimeoutSeconds = *Timeout;
        }
        else
        {
            request.ActionOptions.Type = WSLCComposeActionOptionsTypeNone;
        }

        auto callback = Microsoft::WRL::Make<ComposeProgressCallback>(Terminal);
        THROW_IF_NULL_ALLOC(callback);

        wil::com_ptr<IComposeOperation> operation;
        THROW_IF_FAILED(Session.Get()->BeginComposeOperation(&request, callback.Get(), &operation));

        wil::unique_handle completionEvent;
        THROW_IF_FAILED(operation->GetCompletionEvent(&completionEvent));

        const std::array handles{completionEvent.get(), CancelEvent};
        const DWORD handleCount = CancelEvent == nullptr ? 1 : static_cast<DWORD>(handles.size());
        const DWORD waitResult = WaitForMultipleObjects(handleCount, handles.data(), FALSE, INFINITE);
        THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
        THROW_HR_IF(E_UNEXPECTED, waitResult < WAIT_OBJECT_0 || waitResult >= WAIT_OBJECT_0 + handleCount);
        if (CancelEvent != nullptr && waitResult == WAIT_OBJECT_0 + 1)
        {
            THROW_IF_FAILED(operation->Cancel());
            THROW_LAST_ERROR_IF(WaitForSingleObject(completionEvent.get(), INFINITE) == WAIT_FAILED);
        }

        ComposeOperationResult result;
        THROW_IF_FAILED(operation->GetResult(&result.value));
        THROW_IF_FAILED(result.value.Result);
        THROW_HR_IF(E_UNEXPECTED, result.value.Status != WSLCComposeOperationStatusSucceeded);
        return result;
    }

    int AttachContainers(models::Session& Session, const ComposeOperationResult& Result, HANDLE CancelEvent)
    {
        [[maybe_unused]] auto operation = Session.BeginContainerOperation();
        common::io::MultiHandleWait io;

        if (CancelEvent != nullptr)
        {
            io.AddHandle(
                std::make_unique<common::io::EventHandle>(CancelEvent),
                common::io::MultiHandleWait::CancelOnCompleted | common::io::MultiHandleWait::NeedNotComplete);
        }

        for (ULONG index = 0; index < Result.value.AffectedContainersCount; ++index)
        {
            const auto& entry = Result.value.AffectedContainers[index];
            wil::com_ptr<IWSLCContainer> container;
            THROW_IF_FAILED(Session.Get()->OpenContainer(entry.Id, &container));

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

std::vector<models::ComposeProjectInformation> ComposeService::List(models::Session& Session, bool All)
{
    WSLCComposeProjectListOptions options{};
    options.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    options.All = All;

    wil::unique_cotaskmem_array_ptr<WSLCComposeProjectSummary> projects;
    THROW_IF_FAILED(Session.Get()->ListComposeProjects(&options, &projects, projects.size_address<ULONG>()));

    std::vector<models::ComposeProjectInformation> result;
    result.reserve(projects.size());
    for (const auto& project : projects)
    {
        THROW_HR_IF(E_UNEXPECTED, project.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
        result.emplace_back(project.ProjectKey, FormatProjectStatus(project));
    }

    return result;
}

void ComposeService::Create(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent)
{
    Execute(Terminal, Session, Path, WSLCComposeActionCreate, CancelEvent);
}

int ComposeService::Up(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent)
{
    auto result = Execute(Terminal, Session, Path, WSLCComposeActionUp, CancelEvent);
    return AttachContainers(Session, result, CancelEvent);
}

void ComposeService::Start(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent)
{
    Execute(Terminal, Session, Path, WSLCComposeActionStart, CancelEvent);
}

int ComposeService::Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent)
{
    auto result = Execute(Terminal, Session, Path, WSLCComposeActionAttach, CancelEvent);
    return AttachContainers(Session, result, CancelEvent);
}

void ComposeService::Stop(Terminal& Terminal, models::Session& Session, const std::wstring& Path, ULONG Timeout, HANDLE CancelEvent)
{
    Execute(Terminal, Session, Path, WSLCComposeActionStop, CancelEvent, Timeout);
}

} // namespace wsl::windows::wslc::services
