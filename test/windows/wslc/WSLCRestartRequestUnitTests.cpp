// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCExitRequestState.h"
#include "../../../src/windows/wslcsession/WSLCRestartBackoff.h"
#include "../../../src/windows/wslcsession/WSLCRestartTransaction.h"

namespace WSLCRestartRequests {

using wsl::windows::service::wslc::WSLCExitRequestState;
using wsl::windows::service::wslc::WSLCRestartTransaction;

class WSLCRestartRequestUnitTests
{
    WSLC_TEST_CLASS(WSLCRestartRequestUnitTests)

    TEST_METHOD(RestartCleanup_CancelledRestartCannotCleanUpReplacement)
    {
        auto cancelled = std::make_shared<WSLCRestartTransaction>();
        auto owner = cancelled;

        // A's start commits before its cancelled wait unwinds; B acquires the stopped interval.
        owner.reset();
        auto replacement = std::make_shared<WSLCRestartTransaction>();
        owner = replacement;

        unsigned cleanupCalls = 0;
        cancelled->CleanupIfOwned(owner, false, [&]() { ++cleanupCalls; });
        VERIFY_ARE_EQUAL(0u, cleanupCalls);
        VERIFY_IS_TRUE(owner == replacement);

        replacement->CleanupIfOwned(owner, false, [&]() {
            VERIFY_IS_FALSE(static_cast<bool>(owner));
            ++cleanupCalls;
        });
        VERIFY_ARE_EQUAL(1u, cleanupCalls);
        VERIFY_IS_FALSE(static_cast<bool>(owner));
    }

    TEST_METHOD(RestartCleanup_CancelledWaitAfterStartCommitDoesNotCleanUp)
    {
        auto restart = std::make_shared<WSLCRestartTransaction>();
        auto owner = restart;
        owner.reset();

        unsigned cleanupCalls = 0;
        restart->CleanupIfOwned(owner, false, [&]() { ++cleanupCalls; });
        VERIFY_ARE_EQUAL(0u, cleanupCalls);
        VERIFY_IS_FALSE(static_cast<bool>(owner));
    }

    TEST_METHOD(RestartCleanup_SuccessReleasesOwnershipWithoutFailureCleanup)
    {
        auto restart = std::make_shared<WSLCRestartTransaction>();
        auto owner = restart;

        unsigned cleanupCalls = 0;
        restart->CleanupIfOwned(owner, true, [&]() { ++cleanupCalls; });
        VERIFY_ARE_EQUAL(0u, cleanupCalls);
        VERIFY_IS_FALSE(static_cast<bool>(owner));
    }

    TEST_METHOD(RestartBackoff_CapturesEachRunDuration)
    {
        wsl::windows::service::wslc::WSLCRestartBackoff backoff;
        const auto start = std::chrono::steady_clock::time_point{};
        backoff.OnStarted(start);
        backoff.OnExited(start + 1s);
        VERIFY_ARE_EQUAL(100, backoff.NextDelay().count());

        backoff.OnStarted(start + 2s);
        backoff.OnExited(start + 3s);
        VERIFY_ARE_EQUAL(200, backoff.NextDelay().count());

        backoff.OnStarted(start + 30s);
        backoff.OnExited(start + 31s);
        VERIFY_ARE_EQUAL(400, backoff.NextDelay().count());
    }

    TEST_METHOD(RestartBackoff_ExactResetThresholdAndCap)
    {
        wsl::windows::service::wslc::WSLCRestartBackoff backoff;
        const auto start = std::chrono::steady_clock::time_point{};
        backoff.OnStarted(start);
        backoff.OnExited(start + 9999ms);
        for (const auto delay : {100ms, 200ms, 400ms, 800ms, 1600ms, 3200ms, 6400ms, 12800ms, 25600ms, 51200ms, 60000ms, 60000ms})
        {
            VERIFY_ARE_EQUAL(delay.count(), backoff.NextDelay().count());
        }

        backoff.OnStarted(start + 20s);
        backoff.OnExited(start + 30s);
        VERIFY_ARE_EQUAL(100, backoff.NextDelay().count());

        backoff.OnStarted(start + 40s);
        backoff.OnExited(start + 41s);
        VERIFY_ARE_EQUAL(200, backoff.NextDelay().count());
        backoff.Reset();
        VERIFY_ARE_EQUAL(100, backoff.NextDelay().count());
    }

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
            VERIFY_IS_TRUE(state.OwnsRun(oldRequest));

            // The replacement is staged before its Start event commits.
            state.BeginRun();
            VERIFY_IS_FALSE(state.OwnsRun(oldRequest));
            VERIFY_IS_FALSE(state.CompleteRequest(oldRequest, succeeded));
            VERIFY_IS_FALSE(state.HasPendingRequests());
            VERIFY_IS_FALSE(state.SuppressesRestart());

            const auto newRequest = state.BeginRequest();
            VERIFY_IS_TRUE(state.OwnsRun(newRequest));
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
