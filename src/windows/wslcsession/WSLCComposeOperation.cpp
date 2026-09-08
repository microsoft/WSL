// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCComposeOperation.h"
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

    m_request = CaptureRequest(*request);
    m_session = session;
    m_sessionLifetime = session;
    if (progressCallback != nullptr)
    {
        m_git = session->m_git;
        THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(progressCallback.Get(), __uuidof(IComposeProgressCallback), &m_progressCallbackGitCookie));
    }

    m_cancelEvent.create(wil::EventOptions::ManualReset);
    m_completionEvent.create(wil::EventOptions::ManualReset);

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

    if (!m_affectedContainers.empty())
    {
        auto containers = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(m_affectedContainers.size());
        std::ranges::copy(m_affectedContainers, containers.get());
        result->AffectedContainers = containers.release();
        result->AffectedContainersCount = static_cast<ULONG>(m_affectedContainers.size());
    }

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
    result.Selection.Profiles = CaptureStrings(request.Selection.Profiles);
    result.Selection.Services = CaptureStrings(request.Selection.Services);
    result.Selection.IncludeDependencies = request.Selection.IncludeDependencies;
    result.ActionOptions = request.ActionOptions;

    switch (request.Project.Type)
    {
    case WSLCComposeProjectTypeDocuments:
    {
        THROW_HR_IF_NULL(E_POINTER, request.Project.Value.Documents);
        const auto& source = *request.Project.Value.Documents;
        THROW_HR_IF_NULL(E_POINTER, source.WorkingDirectory);
        THROW_HR_IF_NULL(E_POINTER, source.ProjectDirectory);
        THROW_HR_IF(E_INVALIDARG, source.DocumentCount == 0);
        THROW_HR_IF_NULL(E_POINTER, source.Documents);

        ComposeDocuments documents{
            .SchemaVersion = source.SchemaVersion,
            .WorkingDirectory = source.WorkingDirectory,
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

std::vector<std::string> WSLCComposeOperation::CaptureStrings(const WSLCStringArray& values)
{
    THROW_HR_IF(E_POINTER, values.Count != 0 && values.Values == nullptr);

    std::vector<std::string> result;
    result.reserve(values.Count);
    for (ULONG index = 0; index < values.Count; ++index)
    {
        THROW_HR_IF_NULL(E_POINTER, values.Values[index]);
        result.emplace_back(values.Values[index]);
    }

    return result;
}

void WSLCComposeOperation::Run() noexcept
{
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    auto signalCompletion = wil::scope_exit([&]() { m_completionEvent.SetEvent(); });

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
    const HRESULT result = wil::ResultFromException([&]() {
        if (m_progressCallbackGitCookie != 0)
        {
            THROW_IF_FAILED(m_git->GetInterfaceFromGlobal(
                m_progressCallbackGitCookie, __uuidof(IComposeProgressCallback), progressCallback.put_void()));
        }

        executionResult = RunOperation(progressCallback.get(), projectKey);
    });
    const bool cancelled = result == HRESULT_FROM_WIN32(ERROR_CANCELLED);

    if (FAILED(result))
    {
        ReportStatusNoThrow(progressCallback.get(), cancelled ? WSLCComposeStatusCancelled : WSLCComposeStatusFailed);
    }

    std::lock_guard resultLock(m_resultLock);
    m_resultCode = cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : result;
    m_status = cancelled           ? WSLCComposeOperationStatusCancelled
               : SUCCEEDED(result) ? WSLCComposeOperationStatusSucceeded
                                   : WSLCComposeOperationStatusFailed;
    m_projectKey = std::move(projectKey);
    m_affectedContainers = std::move(executionResult.AffectedContainers);
}

ComposeExecutionResult WSLCComposeOperation::RunOperation(IComposeProgressCallback* progressCallback, std::string& projectKey)
{
    ReportStatus(progressCallback, WSLCComposeStatusValidating);
    CheckCancelled();
    ComposeNormalizer::ValidateSelection(m_request.Selection);

    std::optional<ComposeSpec> desiredProject;
    if (m_request.ProjectType == WSLCComposeProjectTypeDocuments)
    {
        desiredProject = ComposeNormalizer::Normalize(*m_request.Documents, m_request.Selection);
        projectKey = desiredProject->ProjectName;
    }
    else
    {
        projectKey = ComposeNormalizer::ValidateProjectKey(m_request.ProjectKey);
    }

    CheckCancelled();
    if (m_request.Action == WSLCComposeActionValidate)
    {
        ReportStatus(progressCallback, WSLCComposeStatusSucceeded);
        return {.ProjectKey = projectKey};
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
        m_request.Action, desiredProject ? &*desiredProject : nullptr, projectKey, m_request.ActionOptions, m_cancelEvent.get(), progressReporter);
    ReportStatus(progressCallback, WSLCComposeStatusSucceeded);
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
    LOG_IF_FAILED(wil::ResultFromException([&]() { ReportStatus(progressCallback, status); }));
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

    auto callbackRegistration = m_session->RegisterUserCOMCallback();
    THROW_IF_FAILED(progressCallback->OnProgress(&event));
}

} // namespace wsl::windows::service::wslc
