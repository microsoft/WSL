// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeNormalizer.h"
#include "ComposeReconciler.h"
#include "ExecutionContext.h"
#include "wslc.h"

namespace wsl::windows::service::wslc {

class WSLCSession;
class WSLCComposeOperation
{
public:
    NON_COPYABLE(WSLCComposeOperation);
    NON_MOVABLE(WSLCComposeOperation);

    WSLCComposeOperation(WSLCSession& session, const WSLCComposeOperationRequest& request, IComposeProgressCallback* progressCallback, HANDLE cancelEvent);
    void Execute(WSLCComposeOperationResult& result);

private:
    struct Request
    {
        WSLCComposeAction Action{};
        WSLCComposeProjectType ProjectType{};
        std::optional<ComposeDocuments> Documents;
        std::string ProjectKey;
        WSLCComposeActionOptions ActionOptions{};
    };

    static Request CaptureRequest(const WSLCComposeOperationRequest& request);
    ComposeExecutionResult RunOperation(std::string& projectKey, std::optional<wsl::windows::common::Error>& primaryError);
    void ReportStatus(WSLCComposeStatus status);
    void ReportStatusNoThrow(WSLCComposeStatus status) noexcept;
    void ReportProgress(std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit);

    WSLCSession& m_session;
    Request m_request;
    wil::com_ptr<IComposeProgressCallback> m_progressCallback;
    ComposeCancellation m_cancellation;
    ULONGLONG m_sequenceNumber{};
};

} // namespace wsl::windows::service::wslc
