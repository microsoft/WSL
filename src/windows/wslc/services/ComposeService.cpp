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

    constexpr ULONG c_defaultStopTimeoutSeconds = 10;

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
            sourcePath = std::filesystem::absolute(path).lexically_normal();
            baseDirectory = sourcePath.parent_path();
            projectDirectory = baseDirectory;
            workingDirectory = std::filesystem::current_path();

            std::ifstream stream{sourcePath, std::ios::binary | std::ios::ate};
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream, "Failed to open compose document %ls", sourcePath.c_str());

            const auto documentSize = stream.tellg();
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                documentSize < 0,
                "Failed to determine compose document size: %ls",
                sourcePath.c_str());
            THROW_HR_IF(E_INVALIDARG, documentSize == 0);
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                static_cast<uint64_t>(documentSize) > ULONG_MAX,
                "Compose document exceeds the maximum transport size: %ls",
                sourcePath.c_str());

            content.resize(static_cast<size_t>(documentSize));
            stream.seekg(0, std::ios::beg);
            stream.read(content.data(), static_cast<std::streamsize>(content.size()));
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                stream.gcount() != static_cast<std::streamsize>(content.size()),
                "Failed to read compose document: %ls",
                sourcePath.c_str());

            char extraByte{};
            stream.read(&extraByte, 1);
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_READ_FAULT),
                stream.gcount() != 0,
                "Compose document changed while it was being read: %ls",
                sourcePath.c_str());

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

        ComposeOperationResult(ComposeOperationResult&& other) noexcept : value(other.value)
        {
            other.value.AffectedContainers = nullptr;
            other.value.AffectedContainersCount = 0;
        }

        ~ComposeOperationResult()
        {
            CoTaskMemFree(value.AffectedContainers);
        }

        WSLCComposeOperationResult value{};
    };

    void ForceKillContainers(Terminal& terminal, models::Session& session, const ComposeOperationResult& result)
    {
        [[maybe_unused]] auto operation = session.BeginContainerOperation();
        std::vector<std::pair<std::string, wil::com_ptr<IWSLCContainer>>> running;
        for (ULONG index = 0; index < result.value.AffectedContainersCount; ++index)
        {
            const auto& entry = result.value.AffectedContainers[index];
            wil::com_ptr<IWSLCContainer> container;
            const HRESULT openResult = session.Get()->OpenContainer(entry.Id, &container);
            if (openResult == WSLC_E_CONTAINER_NOT_FOUND)
            {
                continue;
            }
            THROW_IF_FAILED(openResult);

            WSLCContainerState state{};
            THROW_IF_FAILED(container->GetState(&state));
            if (state == WslcContainerStateRunning)
            {
                running.emplace_back(entry.Name, std::move(container));
            }
        }

        for (size_t index = 0; index < running.size(); ++index)
        {
            const auto& [name, container] = running[index];
            terminal.Info(
                L"{}\n",
                wsl::shared::Localization::MessageWslcComposeProgress(
                    wsl::shared::Localization::WSLCCLI_ComposeProgressKilling(),
                    wsl::shared::Localization::MessageWslcComposeResource(
                        wsl::shared::Localization::WSLCCLI_ComposeResourceContainer(), wsl::shared::string::MultiByteToWide(name)),
                    index + 1,
                    running.size()));

            const HRESULT killResult = container->Kill(WSLCSignalSIGKILL);
            THROW_IF_FAILED_EXCEPT(killResult, WSLC_E_CONTAINER_NOT_RUNNING);
        }
    }

    ComposeOperationResult Execute(
        Terminal& terminal,
        models::Session& session,
        const ComposeProjectReference& project,
        WSLCComposeAction action,
        HANDLE cancelEvent,
        std::optional<ULONG> timeout = std::nullopt,
        HANDLE forceCancelEvent = nullptr,
        std::function<void()> forceAction = {})
    {
        std::optional<CapturedComposeDocuments> captured;
        WSLCComposeOperationRequest request{};
        request.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
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

        auto callback = Microsoft::WRL::Make<ComposeProgressCallback>(terminal);
        THROW_IF_NULL_ALLOC(callback);

        wil::com_ptr<IComposeOperation> operation;
        THROW_IF_FAILED(session.Get()->BeginComposeOperation(&request, callback.Get(), &operation));

        wil::unique_handle completionEvent;
        THROW_IF_FAILED(operation->GetCompletionEvent(&completionEvent));

        std::array<HANDLE, 3> handles{completionEvent.get()};
        DWORD handleCount = 1;
        std::optional<DWORD> cancelIndex;
        std::optional<DWORD> forceCancelIndex;
        if (cancelEvent != nullptr)
        {
            cancelIndex = handleCount;
            handles[handleCount++] = cancelEvent;
        }
        if (forceCancelEvent != nullptr)
        {
            forceCancelIndex = handleCount;
            handles[handleCount++] = forceCancelEvent;
        }

        const DWORD waitResult = WaitForMultipleObjects(handleCount, handles.data(), FALSE, INFINITE);
        THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
        THROW_HR_IF(E_UNEXPECTED, waitResult < WAIT_OBJECT_0 || waitResult >= WAIT_OBJECT_0 + handleCount);
        const DWORD signaledIndex = waitResult - WAIT_OBJECT_0;
        if (cancelIndex.has_value() && signaledIndex == *cancelIndex)
        {
            THROW_IF_FAILED(operation->Cancel());
            THROW_LAST_ERROR_IF(WaitForSingleObject(completionEvent.get(), INFINITE) == WAIT_FAILED);
        }
        else if (forceCancelIndex.has_value() && signaledIndex == *forceCancelIndex)
        {
            if (forceAction)
            {
                forceAction();
            }
            THROW_LAST_ERROR_IF(WaitForSingleObject(completionEvent.get(), INFINITE) == WAIT_FAILED);
        }

        ComposeOperationResult result;
        THROW_IF_FAILED(operation->GetResult(&result.value));
        THROW_IF_FAILED(result.value.Result);
        THROW_HR_IF(E_UNEXPECTED, result.value.Status != WSLCComposeOperationStatusSucceeded);
        return result;
    }

    int AttachContainers(models::Session& session, const ComposeOperationResult& result, HANDLE cancelEvent)
    {
        [[maybe_unused]] auto operation = session.BeginContainerOperation();
        common::io::MultiHandleWait io;

        if (cancelEvent != nullptr)
        {
            io.AddHandle(
                std::make_unique<common::io::EventHandle>(cancelEvent),
                common::io::MultiHandleWait::CancelOnCompleted | common::io::MultiHandleWait::NeedNotComplete);
        }

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
    options.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    options.All = all;

    wil::unique_cotaskmem_array_ptr<WSLCComposeProjectSummary> projects;
    THROW_IF_FAILED(session.Get()->ListComposeProjects(&options, &projects, projects.size_address<ULONG>()));

    std::vector<models::ComposeProjectInformation> result;
    result.reserve(projects.size());
    for (const auto& project : projects)
    {
        THROW_HR_IF(E_UNEXPECTED, project.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
        result.emplace_back(project.ProjectKey, FormatProjectStatus(project));
    }

    return result;
}

void ComposeService::Create(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent)
{
    Execute(terminal, session, ComposeProjectReference{std::filesystem::path{path}}, WSLCComposeActionCreate, cancelEvent);
}

int ComposeService::Up(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent, HANDLE forceCancelEvent)
{
    const ComposeProjectReference project{std::filesystem::path{path}};
    std::optional<ComposeOperationResult> result;
    auto stopOnCancellation = wil::scope_exit([&]() {
        LOG_IF_FAILED(wil::ResultFromException([&]() {
            if (cancelEvent == nullptr)
            {
                return;
            }

            const auto waitResult = WaitForSingleObject(cancelEvent, 0);
            THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
            THROW_HR_IF(E_UNEXPECTED, waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT);
            if (waitResult == WAIT_OBJECT_0)
            {
                terminal.Info(L"\n{}\n", wsl::shared::Localization::WSLCCLI_ComposeGracefullyStopping());
                Execute(terminal, session, project, WSLCComposeActionStop, nullptr, c_defaultStopTimeoutSeconds, forceCancelEvent, [&]() {
                    if (result.has_value())
                    {
                        ForceKillContainers(terminal, session, *result);
                    }
                });
            }
        }));
    });

    result.emplace(Execute(terminal, session, project, WSLCComposeActionUp, cancelEvent));
    return AttachContainers(session, *result, cancelEvent);
}

void ComposeService::Start(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    Execute(terminal, session, project, WSLCComposeActionStart, cancelEvent);
}

int ComposeService::Attach(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    auto result = Execute(terminal, session, project, WSLCComposeActionAttach, cancelEvent);
    return AttachContainers(session, result, cancelEvent);
}

void ComposeService::Stop(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, ULONG timeout, HANDLE cancelEvent)
{
    Execute(terminal, session, project, WSLCComposeActionStop, cancelEvent, timeout);
}

void ComposeService::Remove(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent)
{
    Execute(terminal, session, project, WSLCComposeActionRemove, cancelEvent);
}

} // namespace wsl::windows::wslc::services
