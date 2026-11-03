// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCComposeOperation.h"
#include "WSLCExecutionContext.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

WSLCComposeOperation::~WSLCComposeOperation()
{
    if (m_cancelEvent)
    {
        m_cancelEvent.SetEvent();
    }

    if (m_worker.joinable())
    {
        if (m_worker.get_id() == std::this_thread::get_id())
        {
            // Releasing the worker's final reference can run destruction on the worker itself.
            m_worker.detach();
        }
        else
        {
            m_worker.join();
        }
    }

    if (m_progressCallbackGitCookie != 0)
    {
        LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(m_progressCallbackGitCookie));
    }
}

HRESULT WSLCComposeOperation::RuntimeClassInitialize(
    WSLCSession* session, const WSLCComposeOperationRequest* request, Microsoft::WRL::ComPtr<IComposeProgressCallback> progressCallback)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, session);
    RETURN_HR_IF_NULL(E_POINTER, request);

    // The worker owns its input snapshot after the incoming COM call releases its request buffers.
    m_request = CaptureRequest(*request);
    m_session = session;
    m_sessionLifetime = session;
    if (progressCallback != nullptr)
    {
        // The global interface table marshals the callback into the worker's COM apartment.
        m_git = session->m_git;
        THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(progressCallback.Get(), __uuidof(IComposeProgressCallback), &m_progressCallbackGitCookie));
    }

    m_cancelEvent.create(wil::EventOptions::ManualReset);
    m_completionEvent.create(wil::EventOptions::ManualReset);

    // The worker holds a reference through Run even if the client releases its operation interface.
    AddRef();
    try
    {
        m_worker = std::thread([this]() {
            Run();
            Release();
        });
    }
    catch (...)
    {
        Release();
        throw;
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeOperation::GetCompletionEvent(HANDLE* event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, event);
    *event = wsl::windows::common::wslutil::DuplicateHandle(m_completionEvent.get(), SYNCHRONIZE, FALSE);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeOperation::Cancel()
{
    m_cancelEvent.SetEvent();
    return S_OK;
}

HRESULT WSLCComposeOperation::GetResult(WSLCComposeOperationResult* result)
try
{
    RETURN_HR_IF_NULL(E_POINTER, result);
    *result = {};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE), !m_completionEvent.is_signaled());

    std::lock_guard resultLock(m_resultLock);
    result->SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    result->Status = m_status;
    result->Result = m_resultCode;
    THROW_HR_IF(E_UNEXPECTED, strcpy_s(result->ProjectKey, m_projectKey.c_str()) != 0);

    wil::unique_cotaskmem_string errorMessage;
    if (m_errorMessage.has_value())
    {
        errorMessage = wil::make_unique_string<wil::unique_cotaskmem_string>(m_errorMessage->c_str());
    }

    if (!m_affectedContainers.empty())
    {
        THROW_HR_IF(E_UNEXPECTED, m_affectedContainers.size() > ULONG_MAX);
        const auto affectedContainersCount = static_cast<ULONG>(m_affectedContainers.size());
        auto containers = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(affectedContainersCount);
        std::ranges::copy(m_affectedContainers, containers.get());
        result->AffectedContainers = containers.release();
        result->AffectedContainersCount = affectedContainersCount;
    }

    result->ErrorMessage = errorMessage.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeOperation::InterfaceSupportsErrorInfo(REFIID interfaceId)
{
    return interfaceId == __uuidof(IComposeOperation) ? S_OK : S_FALSE;
}

WSLCComposeOperation::Request WSLCComposeOperation::CaptureRequest(const WSLCComposeOperationRequest& request)
{
    THROW_HR_IF(E_INVALIDARG, request.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
    THROW_HR_IF(E_INVALIDARG, request.Action < WSLCComposeActionValidate || request.Action > WSLCComposeActionRemove);

    if (request.Action == WSLCComposeActionStop)
    {
        THROW_HR_IF(E_INVALIDARG, request.ActionOptions.Type != WSLCComposeActionOptionsTypeStop);
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, request.ActionOptions.Type != WSLCComposeActionOptionsTypeNone);
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
        THROW_HR_IF(E_INVALIDARG, source.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
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

void WSLCComposeOperation::Run() noexcept
{
    // Initialization failures also publish a terminal result before releasing waiters.
    auto signalCompletion = wil::scope_exit([&]() { m_completionEvent.SetEvent(); });
    const HRESULT result = wil::ResultFromException([&]() { RunInitialized(); });
    if (FAILED(result))
    {
        std::lock_guard resultLock(m_resultLock);
        m_resultCode = result;
        m_status = WSLCComposeOperationStatusFailed;
    }
}

void WSLCComposeOperation::RunInitialized()
{
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    WSLCExecutionContext context(m_session);

    wil::com_ptr<IComposeProgressCallback> progressCallback;
    auto revokeProgressCallback = wil::scope_exit([&]() {
        if (m_progressCallbackGitCookie != 0)
        {
            LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(m_progressCallbackGitCookie));
            m_progressCallbackGitCookie = 0;
        }
    });

    ComposeExecutionResult executionResult;
    std::string projectKey;
    std::optional<wsl::windows::common::Error> primaryError;
    const HRESULT result = wil::ResultFromException([&]() {
        if (m_progressCallbackGitCookie != 0)
        {
            THROW_IF_FAILED(m_git->GetInterfaceFromGlobal(
                m_progressCallbackGitCookie, __uuidof(IComposeProgressCallback), progressCallback.put_void()));
        }

        executionResult = RunOperation(progressCallback.get(), projectKey, primaryError);
    });
    // The execution outcome determines cancellation; a request after the final checkpoint can leave successful work.
    const bool cancelled = result == HRESULT_FROM_WIN32(ERROR_CANCELLED);

    std::optional<std::wstring> errorMessage;
    const auto& error = primaryError.has_value() && primaryError->Code == result ? primaryError : context.ReportedError();
    if (FAILED(result) && error.has_value() && error->Code == result)
    {
        // COM error information is thread-local; result retrieval needs an owned copy of the worker's message.
        LOG_IF_FAILED(wil::ResultFromException([&]() { errorMessage = error->Message; }));
    }

    if (FAILED(result))
    {
        if (!cancelled)
        {
            ReportFailureDiagnosticNoThrow(progressCallback.get(), result, errorMessage);
        }
        ReportStatusNoThrow(progressCallback.get(), cancelled ? WSLCComposeStatusCancelled : WSLCComposeStatusFailed);
    }

    std::lock_guard resultLock(m_resultLock);
    m_resultCode = cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : result;
    m_errorMessage = std::move(errorMessage);
    m_status = cancelled           ? WSLCComposeOperationStatusCancelled
               : SUCCEEDED(result) ? WSLCComposeOperationStatusSucceeded
                                   : WSLCComposeOperationStatusFailed;
    m_projectKey = std::move(projectKey);
    m_affectedContainers = std::move(executionResult.AffectedContainers);
}

ComposeExecutionResult WSLCComposeOperation::RunOperation(
    IComposeProgressCallback* progressCallback, std::string& projectKey, std::optional<wsl::windows::common::Error>& primaryError)
{
    ReportStatus(progressCallback, WSLCComposeStatusValidating);
    CheckCancelled();

    std::optional<ComposeSpec> desiredProject;
    if (m_request.ProjectType == WSLCComposeProjectTypeDocuments)
    {
        desiredProject = ComposeNormalizer::Normalize(*m_request.Documents, {});
        projectKey = desiredProject->ProjectName;
    }
    else
    {
        projectKey = ComposeNormalizer::ValidateProjectKey(m_request.ProjectKey);
    }

    CheckCancelled();
    if (m_request.Action == WSLCComposeActionValidate)
    {
        ReportStatusNoThrow(progressCallback, WSLCComposeStatusSucceeded);
        return {};
    }

    ReportStatus(progressCallback, WSLCComposeStatusPlanning);
    CheckCancelled();
    ReportStatus(progressCallback, WSLCComposeStatusExecuting);

    ComposeProgressReporter progressReporter;
    if (progressCallback != nullptr)
    {
        progressReporter = [this, progressCallback](
                               std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit) {
            ReportProgress(progressCallback, operation, resourceKey, current, total, unit);
        };
    }

    auto result = m_session->m_composeReconciler.Execute(
        m_request.Action, desiredProject ? &*desiredProject : nullptr, projectKey, m_request.ActionOptions, m_cancelEvent.get(), progressReporter, primaryError);
    switch (m_request.Action)
    {
    case WSLCComposeActionUp:
    case WSLCComposeActionAttach:
        ReportStreams(progressCallback, result);
        break;
    case WSLCComposeActionValidate:
    case WSLCComposeActionCreate:
    case WSLCComposeActionStart:
    case WSLCComposeActionStop:
    case WSLCComposeActionRemove:
        break;
    }
    ReportStatusNoThrow(progressCallback, WSLCComposeStatusSucceeded);
    return result;
}

void WSLCComposeOperation::CheckCancelled() const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), m_cancelEvent.is_signaled());
}

void WSLCComposeOperation::ReportStatus(IComposeProgressCallback* progressCallback, WSLCComposeStatus status)
{
    if (progressCallback == nullptr)
    {
        return;
    }

    WSLCComposeProgressEvent event{};
    event.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    event.SequenceNumber = ++m_sequenceNumber;
    event.Kind = WSLCComposeProgressEventKindStatus;
    event.Value.Status.Status = status;

    auto callbackRegistration = m_session->RegisterUserCOMCallback();
    THROW_IF_FAILED(progressCallback->OnProgress(&event));
}

void WSLCComposeOperation::ReportStatusNoThrow(IComposeProgressCallback* progressCallback, WSLCComposeStatus status) noexcept
{
    // Terminal notification ignores returned errors but still waits for the callback to return.
    LOG_IF_FAILED(wil::ResultFromException([&]() { ReportStatus(progressCallback, status); }));
}

void WSLCComposeOperation::ReportFailureDiagnosticNoThrow(
    IComposeProgressCallback* progressCallback, HRESULT result, const std::optional<std::wstring>& errorMessage) noexcept
{
    if (progressCallback == nullptr)
    {
        return;
    }

    LOG_IF_FAILED(wil::ResultFromException([&]() {
        const auto resultCode = std::format("0x{:08X}", static_cast<ULONG>(result));
        const auto message = errorMessage.has_value() ? wsl::shared::string::WideToMultiByte(*errorMessage) : std::string{};
        const std::array<LPCSTR, 2> arguments{resultCode.c_str(), message.c_str()};
        WSLCComposeProgressEvent event{};
        event.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
        event.SequenceNumber = ++m_sequenceNumber;
        event.Kind = WSLCComposeProgressEventKindDiagnostic;
        event.Value.Diagnostic.Severity = WSLCComposeDiagnosticSeverityError;
        event.Value.Diagnostic.Code = WSLC_COMPOSE_DIAGNOSTIC_OPERATION_FAILED;
        event.Value.Diagnostic.Arguments = {.Values = arguments.data(), .Count = static_cast<ULONG>(arguments.size())};
        if (m_request.Documents.has_value())
        {
            event.Value.Diagnostic.SourcePath = m_request.Documents->Documents.front().SourcePath.c_str();
        }

        auto callbackRegistration = m_session->RegisterUserCOMCallback();
        THROW_IF_FAILED(progressCallback->OnProgress(&event));
    }));
}

void WSLCComposeOperation::ReportStreams(IComposeProgressCallback* progressCallback, const ComposeExecutionResult& result)
{
    if (progressCallback == nullptr)
    {
        return;
    }

    // Attachment covers running project containers and succeeds when none are running.
    // TODO: Align standalone attach with Docker Compose service selection and its error when no target is running.
    // TODO: Align attached up with Docker Compose attachment timing and log fallback for exited containers.
    for (const auto& entry : result.AffectedContainers)
    {
        CheckCancelled();
        if (entry.State != WslcContainerStateRunning)
        {
            continue;
        }

        Microsoft::WRL::ComPtr<IWSLCContainer> container;
        THROW_IF_FAILED(m_session->OpenContainer(entry.Id, &container));
        wsl::windows::common::wslutil::COMOutputHandle stdinHandle;
        wsl::windows::common::wslutil::COMOutputHandle stdoutHandle;
        wsl::windows::common::wslutil::COMOutputHandle stderrHandle;
        const HRESULT attachResult = container->Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle);
        if (attachResult == WSLC_E_CONTAINER_NOT_RUNNING)
        {
            continue;
        }
        THROW_IF_FAILED(attachResult);

        WSLCComposeStreams streams{};
        streams.Stdout = stdoutHandle;
        streams.Stderr = stderrHandle;
        auto callbackRegistration = m_session->RegisterUserCOMCallback();
        THROW_IF_FAILED(progressCallback->OnStreamsReady(&streams));
    }
}

void WSLCComposeOperation::ReportProgress(
    IComposeProgressCallback* progressCallback, std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit)
{
    if (progressCallback == nullptr)
    {
        return;
    }

    const std::string operationValue{operation};
    const std::string resourceKeyValue{resourceKey};
    const std::string unitValue{unit};

    WSLCComposeProgressEvent event{};
    event.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    event.SequenceNumber = ++m_sequenceNumber;
    event.Kind = WSLCComposeProgressEventKindProgress;
    event.Value.Progress.Operation = operationValue.c_str();
    event.Value.Progress.ResourceKey = resourceKeyValue.c_str();
    event.Value.Progress.Current = current;
    event.Value.Progress.Total = total;
    event.Value.Progress.Unit = unitValue.c_str();

    // A stalled resource callback holds the project lock and delays cancellation checkpoints and completion.
    // TODO: Decouple observer delivery from execution and completion.
    auto callbackRegistration = m_session->RegisterUserCOMCallback();
    THROW_IF_FAILED(progressCallback->OnProgress(&event));
}

} // namespace wsl::windows::service::wslc
