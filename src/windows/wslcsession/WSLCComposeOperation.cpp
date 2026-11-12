// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCComposeOperation.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

WSLCComposeOperation::WSLCComposeOperation(
    WSLCSession& session, const WSLCComposeOperationRequest& request, IComposeProgressCallback* progressCallback, HANDLE cancelEvent) :
    m_session(session),
    m_request(CaptureRequest(request)),
    m_progressCallback(progressCallback),
    m_cancellation(session.m_sessionTerminatingEvent.get(), cancelEvent)
{
}

WSLCComposeOperation::Request WSLCComposeOperation::CaptureRequest(const WSLCComposeOperationRequest& request)
{
    switch (request.Action)
    {
    case WSLCComposeActionStop:
        THROW_HR_IF(E_INVALIDARG, request.ActionOptions.Type != WSLCComposeActionOptionsTypeStop);
        break;
    case WSLCComposeActionValidate:
    case WSLCComposeActionCreate:
    case WSLCComposeActionUp:
    case WSLCComposeActionStart:
    case WSLCComposeActionAttach:
        THROW_HR_IF(E_INVALIDARG, request.ActionOptions.Type != WSLCComposeActionOptionsTypeNone);
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    struct Request result;
    result.Action = request.Action;
    result.ProjectType = request.Project.Type;
    result.ActionOptions = request.ActionOptions;

    switch (request.Project.Type)
    {
    case WSLCComposeProjectTypeDocuments:
    {
        THROW_HR_IF_NULL(E_POINTER, request.Project.Value.Documents);
        const auto& source = *request.Project.Value.Documents;
        THROW_HR_IF_NULL(E_POINTER, source.ProjectDirectory);
        THROW_HR_IF(E_INVALIDARG, source.DocumentCount == 0);
        THROW_HR_IF_NULL(E_POINTER, source.Documents);

        ComposeDocuments documents{
            .ProjectDirectory = source.ProjectDirectory,
        };
        if (source.ExplicitProjectName != nullptr)
        {
            documents.ExplicitProjectName = source.ExplicitProjectName;
        }

        documents.Documents.reserve(source.DocumentCount);
        for (ULONG index = 0; index < source.DocumentCount; ++index)
        {
            const auto& document = source.Documents[index];
            THROW_HR_IF_NULL(E_POINTER, document.SourcePath);
            THROW_HR_IF_NULL(E_POINTER, document.BaseDirectory);
            THROW_HR_IF(E_INVALIDARG, document.ContentSize == 0);
            THROW_HR_IF_NULL(E_POINTER, document.Content);

            ComposeDocument captured{
                .SourcePath = document.SourcePath,
                .BaseDirectory = document.BaseDirectory,
            };
            const auto* begin = reinterpret_cast<const std::byte*>(document.Content);
            captured.Content.assign(begin, begin + document.ContentSize);
            documents.Documents.emplace_back(std::move(captured));
        }

        result.Documents = std::move(documents);
        break;
    }

    case WSLCComposeProjectTypeProjectKey:
        THROW_HR_IF_NULL(E_POINTER, request.Project.Value.ProjectKey);
        result.ProjectKey = request.Project.Value.ProjectKey;
        THROW_HR_IF(E_INVALIDARG, request.Action == WSLCComposeActionValidate || request.Action == WSLCComposeActionCreate || request.Action == WSLCComposeActionUp);
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    return result;
}

void WSLCComposeOperation::Execute(WSLCComposeOperationResult& output, const common::ExecutionContext& context)
{
    ComposeExecutionResult executionResult;
    std::string projectKey;
    std::optional<wsl::windows::common::Error> primaryError;
    const HRESULT result = wil::ResultFromException([&]() { executionResult = RunOperation(projectKey, primaryError); });
    // The execution outcome determines cancellation; a request after the final checkpoint can leave successful work.
    const bool cancelled = result == HRESULT_FROM_WIN32(ERROR_CANCELLED);

    wil::unique_cotaskmem_string errorMessage;
    const auto& error = primaryError.has_value() && primaryError->Code == result ? primaryError : context.ReportedError();
    if (FAILED(result) && error.has_value() && error->Code == result && error->Message.has_value())
    {
        // Terminal callbacks can replace thread-local COM error information.
        errorMessage = wil::make_cotaskmem_string(error->Message->c_str());
    }

    if (FAILED(result))
    {
        ReportStatusNoThrow(cancelled ? WSLCComposeStatusCancelled : WSLCComposeStatusFailed);
    }

    WSLCComposeOperationResult completed{};
    completed.Result = result;
    completed.Status = cancelled           ? WSLCComposeOperationStatusCancelled
                       : SUCCEEDED(result) ? WSLCComposeOperationStatusSucceeded
                                           : WSLCComposeOperationStatusFailed;
    THROW_HR_IF(E_UNEXPECTED, strcpy_s(completed.ProjectKey, projectKey.c_str()) != 0);
    if (!executionResult.AffectedContainers.empty())
    {
        completed.AffectedContainersCount = gsl::narrow<ULONG>(executionResult.AffectedContainers.size());
        auto containers = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(completed.AffectedContainersCount);
        std::ranges::copy(executionResult.AffectedContainers, containers.get());
        completed.AffectedContainers = containers.release();
    }

    completed.ErrorMessage = errorMessage.release();
    output = completed;
}

ComposeExecutionResult WSLCComposeOperation::RunOperation(std::string& projectKey, std::optional<wsl::windows::common::Error>& primaryError)
{
    m_cancellation.Check();
    ReportStatus(WSLCComposeStatusValidating);
    m_cancellation.Check();

    std::optional<ComposeSpec> desiredProject;
    switch (m_request.ProjectType)
    {
    case WSLCComposeProjectTypeDocuments:
        desiredProject = ComposeNormalizer::Normalize(*m_request.Documents, {});
        projectKey = desiredProject->ProjectName;
        break;
    case WSLCComposeProjectTypeProjectKey:
        projectKey = ComposeNormalizer::ValidateProjectKey(m_request.ProjectKey);
        break;
    default:
        THROW_HR(E_UNEXPECTED);
    }

    m_cancellation.Check();
    if (m_request.Action == WSLCComposeActionValidate)
    {
        ReportStatusNoThrow(WSLCComposeStatusSucceeded);
        return {};
    }

    ReportStatus(WSLCComposeStatusExecuting);

    ComposeProgressReporter progressReporter;
    if (m_progressCallback)
    {
        progressReporter = [this](std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit) {
            ReportProgress(operation, resourceKey, current, total, unit);
        };
    }

    auto result = m_session.m_composeReconciler.Execute(
        m_request.Action, desiredProject, projectKey, m_request.ActionOptions, m_cancellation, progressReporter, primaryError);
    ReportStatusNoThrow(WSLCComposeStatusSucceeded);
    return result;
}

void WSLCComposeOperation::ReportStatus(WSLCComposeStatus status)
{
    if (!m_progressCallback)
    {
        return;
    }

    WSLCComposeProgressEvent event{};
    event.SequenceNumber = ++m_sequenceNumber;
    event.Status = status;

    auto callbackRegistration = m_session.RegisterUserCOMCallback();
    try
    {
        LOG_IF_FAILED(m_progressCallback->OnProgress(&event));
    }
    CATCH_LOG();
}

void WSLCComposeOperation::ReportStatusNoThrow(WSLCComposeStatus status) noexcept
{
    // Terminal notification ignores returned errors but still waits for the callback to return.
    LOG_IF_FAILED(wil::ResultFromException([&]() { ReportStatus(status); }));
}

void WSLCComposeOperation::ReportProgress(std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit)
{
    if (!m_progressCallback)
    {
        return;
    }

    const std::string operationValue{operation};
    const std::string resourceKeyValue{resourceKey};
    const std::string unitValue{unit};

    const WSLCComposeResourceProgressEvent progress{
        .Operation = operationValue.c_str(),
        .ResourceKey = resourceKeyValue.c_str(),
        .Current = current,
        .Total = total,
        .Unit = unitValue.c_str(),
    };

    WSLCComposeProgressEvent event{};
    event.SequenceNumber = ++m_sequenceNumber;
    event.Status = WSLCComposeStatusExecuting;
    event.Progress = &progress;

    // A stalled resource callback holds the project lock and delays cancellation checkpoints and completion.
    // TODO: Decouple observer delivery from execution and completion.
    auto callbackRegistration = m_session.RegisterUserCOMCallback();
    try
    {
        LOG_IF_FAILED(m_progressCallback->OnProgress(&event));
    }
    CATCH_LOG();
}

} // namespace wsl::windows::service::wslc
