// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCExitRequestState.h"

namespace WSLCRestartRequests {

using wsl::windows::service::wslc::WSLCExitRequestState;

class WSLCRestartRequestUnitTests
{
    WSLC_TEST_CLASS(WSLCRestartRequestUnitTests)

    TEST_METHOD(RestartRequests_StopDuringDeferredExit)
    {
        WSLCExitRequestState state;
        state.BeginRun();
        const auto request = state.BeginRequest();
        VERIFY_IS_TRUE(state.SuppressesRestart());

        state.RequestExit();
        VERIFY_IS_TRUE(state.CompleteRequest(request, false));
        VERIFY_IS_FALSE(state.HasPendingRequests());
        VERIFY_IS_TRUE(state.SuppressesRestart());
    }

    TEST_METHOD(RestartRequests_ReplacementStartInvalidatesOldResponses)
    {
        for (const bool succeeded : {false, true})
        {
            WSLCExitRequestState state;
            state.BeginRun();
            const auto oldRequest = state.BeginRequest();

            // The replacement is staged before its Start event commits.
            state.BeginRun();
            VERIFY_IS_FALSE(state.CompleteRequest(oldRequest, succeeded));
            VERIFY_IS_FALSE(state.HasPendingRequests());
            VERIFY_IS_FALSE(state.SuppressesRestart());

            const auto newRequest = state.BeginRequest();
            VERIFY_IS_FALSE(state.CompleteRequest(oldRequest, succeeded));
            VERIFY_IS_TRUE(state.HasPendingRequests());
            VERIFY_IS_TRUE(state.CompleteRequest(newRequest, false));
            VERIFY_IS_FALSE(state.HasPendingRequests());
            VERIFY_IS_FALSE(state.SuppressesRestart());
        }
    }

    TEST_METHOD(RestartRequests_OverlappingSuccessAndFailure)
    {
        for (const bool successCompletesFirst : {false, true})
        {
            WSLCExitRequestState state;
            state.BeginRun();
            const auto success = state.BeginRequest();
            const auto failure = state.BeginRequest();

            VERIFY_IS_TRUE(state.CompleteRequest(successCompletesFirst ? success : failure, successCompletesFirst));
            VERIFY_IS_TRUE(state.HasPendingRequests());
            VERIFY_IS_TRUE(state.SuppressesRestart());

            VERIFY_IS_TRUE(state.CompleteRequest(successCompletesFirst ? failure : success, !successCompletesFirst));
            VERIFY_IS_FALSE(state.HasPendingRequests());
            VERIFY_IS_TRUE(state.SuppressesRestart());
        }
    }

    TEST_METHOD(RestartRequests_AllRequestsFail)
    {
        WSLCExitRequestState state;
        state.BeginRun();
        const auto first = state.BeginRequest();
        const auto second = state.BeginRequest();
        VERIFY_IS_TRUE(state.CompleteRequest(first, false));
        VERIFY_IS_TRUE(state.SuppressesRestart());
        VERIFY_IS_TRUE(state.CompleteRequest(second, false));
        VERIFY_IS_FALSE(state.SuppressesRestart());
    }

    TEST_METHOD(RestartRequests_FailurePreservesSuccessfulIntent)
    {
        WSLCExitRequestState state;
        state.BeginRun();
        const auto success = state.BeginRequest();
        VERIFY_IS_TRUE(state.CompleteRequest(success, true));
        const auto failure = state.BeginRequest();
        VERIFY_IS_TRUE(state.CompleteRequest(failure, false));
        VERIFY_IS_TRUE(state.SuppressesRestart());

        state.BeginRun();
        VERIFY_IS_FALSE(state.SuppressesRestart());
    }
};

} // namespace WSLCRestartRequests
