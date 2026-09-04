// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCComposeOperation.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

namespace {

    constexpr size_t c_maxComposeDocumentSize = 16 * 1024 * 1024;

} // namespace

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
    WSLCSession* Session, const WSLCComposeOperationRequest* Request, Microsoft::WRL::ComPtr<IComposeProgressCallback> ProgressCallback)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, Session);
    RETURN_HR_IF_NULL(E_POINTER, Request);

    m_request = CaptureRequest(*Request);
    m_session = Session;
    m_sessionLifetime = Session;
    if (ProgressCallback != nullptr)
    {
        m_git = Session->m_git;
        THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(ProgressCallback.Get(), __uuidof(IComposeProgressCallback), &m_progressCallbackGitCookie));
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

HRESULT WSLCComposeOperation::GetCompletionEvent(HANDLE* Event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Event);
    *Event = wsl::windows::common::wslutil::DuplicateHandle(m_completionEvent.get(), SYNCHRONIZE, FALSE);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeOperation::Cancel()
{
    m_cancelEvent.SetEvent();
    return S_OK;
}

HRESULT WSLCComposeOperation::GetResult(WSLCComposeOperationResult* Result)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Result);
    *Result = {};
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE), !m_completionEvent.is_signaled());

    std::lock_guard resultLock(m_resultLock);
    Result->SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    Result->Status = m_status;
    Result->Result = m_resultCode;
    THROW_HR_IF(E_UNEXPECTED, strcpy_s(Result->ProjectKey, m_projectKey.c_str()) != 0);

    if (!m_affectedContainers.empty())
    {
        auto containers = wil::make_unique_cotaskmem<WSLCContainerEntry[]>(m_affectedContainers.size());
        std::ranges::copy(m_affectedContainers, containers.get());
        Result->AffectedContainers = containers.release();
        Result->AffectedContainersCount = static_cast<ULONG>(m_affectedContainers.size());
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCComposeOperation::InterfaceSupportsErrorInfo(REFIID InterfaceId)
{
    return InterfaceId == __uuidof(IComposeOperation) ? S_OK : S_FALSE;
}

WSLCComposeOperation::Request WSLCComposeOperation::CaptureRequest(const WSLCComposeOperationRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, Request.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
    THROW_HR_IF(E_INVALIDARG, Request.Action < WSLCComposeActionValidate || Request.Action > WSLCComposeActionStop);

    if (Request.Action == WSLCComposeActionStop)
    {
        THROW_HR_IF(E_INVALIDARG, Request.ActionOptions.Type != WSLCComposeActionOptionsTypeStop);
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, Request.ActionOptions.Type != WSLCComposeActionOptionsTypeNone);
    }

    struct Request result;
    result.Action = Request.Action;
    result.ProjectType = Request.Project.Type;
    result.Selection.Profiles = CaptureStrings(Request.Selection.Profiles);
    result.Selection.Services = CaptureStrings(Request.Selection.Services);
    result.Selection.IncludeDependencies = Request.Selection.IncludeDependencies;
    result.ActionOptions = Request.ActionOptions;

    switch (Request.Project.Type)
    {
    case WSLCComposeProjectTypeDocuments:
    {
        THROW_HR_IF_NULL(E_POINTER, Request.Project.Value.Documents);
        const auto& source = *Request.Project.Value.Documents;
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
            THROW_HR_IF(E_INVALIDARG, document.ContentSize == 0 || document.ContentSize > c_maxComposeDocumentSize);
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
        THROW_HR_IF_NULL(E_POINTER, Request.Project.Value.ProjectKey);
        result.ProjectKey = Request.Project.Value.ProjectKey;
        THROW_HR_IF(E_INVALIDARG, Request.Action == WSLCComposeActionValidate || Request.Action == WSLCComposeActionCreate || Request.Action == WSLCComposeActionUp);
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    return result;
}

std::vector<std::string> WSLCComposeOperation::CaptureStrings(const WSLCStringArray& Values)
{
    THROW_HR_IF(E_POINTER, Values.Count != 0 && Values.Values == nullptr);

    std::vector<std::string> result;
    result.reserve(Values.Count);
    for (ULONG index = 0; index < Values.Count; ++index)
    {
        THROW_HR_IF_NULL(E_POINTER, Values.Values[index]);
        result.emplace_back(Values.Values[index]);
    }

    return result;
}

void WSLCComposeOperation::Run() noexcept
{
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    auto signalCompletion = wil::scope_exit([&]() { m_completionEvent.SetEvent(); });

    wil::com_ptr<IComposeProgressCallback> progressCallback;
    ComposeExecutionResult executionResult;
    const HRESULT result = wil::ResultFromException([&]() {
        if (m_progressCallbackGitCookie != 0)
        {
            THROW_IF_FAILED(m_git->GetInterfaceFromGlobal(
                m_progressCallbackGitCookie, __uuidof(IComposeProgressCallback), progressCallback.put_void()));
        }

        executionResult = RunOperation(progressCallback.get());
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
    m_projectKey = std::move(executionResult.ProjectKey);
    m_affectedContainers = std::move(executionResult.AffectedContainers);
}

ComposeExecutionResult WSLCComposeOperation::RunOperation(IComposeProgressCallback* ProgressCallback)
{
    ReportStatus(ProgressCallback, WSLCComposeStatusValidating);
    CheckCancelled();
    ComposeNormalizer::ValidateSelection(m_request.Selection);

    std::optional<ComposeSpec> desiredProject;
    std::string projectKey;
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
        ReportStatus(ProgressCallback, WSLCComposeStatusSucceeded);
        return {.ProjectKey = std::move(projectKey)};
    }

    ReportStatus(ProgressCallback, WSLCComposeStatusPlanning);
    CheckCancelled();
    ReportStatus(ProgressCallback, WSLCComposeStatusExecuting);

    auto result = m_session->m_composeReconciler.Execute(
        m_request.Action, desiredProject ? &*desiredProject : nullptr, projectKey, m_request.ActionOptions, m_cancelEvent.get());
    CheckCancelled();
    ReportStatus(ProgressCallback, WSLCComposeStatusSucceeded);
    return result;
}

void WSLCComposeOperation::CheckCancelled() const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), m_cancelEvent.is_signaled());
}

void WSLCComposeOperation::ReportStatus(IComposeProgressCallback* ProgressCallback, WSLCComposeStatus Status)
{
    if (ProgressCallback == nullptr)
    {
        return;
    }

    WSLCComposeProgressEvent event{};
    event.SchemaVersion = WSLC_COMPOSE_SCHEMA_VERSION;
    event.SequenceNumber = ++m_sequenceNumber;
    event.Kind = WSLCComposeProgressEventKindStatus;
    event.Value.Status.Status = Status;

    auto callbackRegistration = m_session->RegisterUserCOMCallback();
    THROW_IF_FAILED(ProgressCallback->OnProgress(&event));
}

void WSLCComposeOperation::ReportStatusNoThrow(IComposeProgressCallback* ProgressCallback, WSLCComposeStatus Status) noexcept
{
    LOG_IF_FAILED(wil::ResultFromException([&]() { ReportStatus(ProgressCallback, Status); }));
}

} // namespace wsl::windows::service::wslc
