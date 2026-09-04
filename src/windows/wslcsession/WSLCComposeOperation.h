// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeNormalizer.h"
#include "wslc.h"
#include <mutex>
#include <thread>

namespace wsl::windows::service::wslc {

class WSLCSession;
struct ComposeExecutionResult;

class DECLSPEC_UUID("0140ECEB-E4DF-46B8-BEA8-37E3FDF3EF3A") WSLCComposeOperation
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IComposeOperation, IFastRundown, ISupportErrorInfo>
{
public:
    WSLCComposeOperation() = default;
    ~WSLCComposeOperation();

    HRESULT RuntimeClassInitialize(WSLCSession* Session, const WSLCComposeOperationRequest* Request, Microsoft::WRL::ComPtr<IComposeProgressCallback> ProgressCallback);

    IFACEMETHOD(GetCompletionEvent)(_Out_ HANDLE* Event) override;
    IFACEMETHOD(Cancel()) override;
    IFACEMETHOD(GetResult)(_Out_ WSLCComposeOperationResult* Result) override;
    IFACEMETHOD(InterfaceSupportsErrorInfo)(_In_ REFIID InterfaceId) override;

private:
    struct Request
    {
        WSLCComposeAction Action{};
        WSLCComposeProjectType ProjectType{};
        std::optional<ComposeDocuments> Documents;
        std::string ProjectKey;
        ComposeProjectSelection Selection;
        WSLCComposeActionOptions ActionOptions{};
    };

    static Request CaptureRequest(const WSLCComposeOperationRequest& Request);
    static std::vector<std::string> CaptureStrings(const WSLCStringArray& Values);
    void Run() noexcept;
    ComposeExecutionResult RunOperation(IComposeProgressCallback* ProgressCallback);
    void CheckCancelled() const;
    void ReportStatus(IComposeProgressCallback* ProgressCallback, WSLCComposeStatus Status);
    void ReportStatusNoThrow(IComposeProgressCallback* ProgressCallback, WSLCComposeStatus Status) noexcept;

    WSLCSession* m_session{};
    Microsoft::WRL::ComPtr<IWSLCSession> m_sessionLifetime;
    wil::com_ptr<IGlobalInterfaceTable> m_git;
    DWORD m_progressCallbackGitCookie{};
    Request m_request;
    wil::unique_event m_cancelEvent;
    wil::unique_event m_completionEvent;
    std::thread m_worker;
    ULONGLONG m_sequenceNumber{};

    std::mutex m_resultLock;
    HRESULT m_resultCode{E_PENDING};
    WSLCComposeOperationStatus m_status{WSLCComposeOperationStatusFailed};
    std::string m_projectKey;
    std::vector<WSLCContainerEntry> m_affectedContainers;
};

} // namespace wsl::windows::service::wslc
