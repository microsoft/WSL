// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "ExecutionContext.h"
#include "wslutil.h"

using namespace wsl::windows::common;

namespace {

class TestServiceExecutionContext : public ServiceExecutionContext
{
public:
    using ServiceExecutionContext::CollectUserWarning;
    using ServiceExecutionContext::ServiceExecutionContext;
};

} // namespace

class ExecutionContextUnitTests
{
    WSL_TEST_CLASS(ExecutionContextUnitTests)

    TEST_METHOD(WarningPipeDuplicationFailurePreservesWarnings)
    {
        auto comCleanup = wil::CoInitializeEx(COINIT_MULTITHREADED);
        wil::com_ptr<IUnknown> callContext;
        VERIFY_ARE_EQUAL(RPC_E_CALL_COMPLETE, ::CoGetCallContext(IID_PPV_ARGS(&callContext)));

        LXSS_ERROR_INFO error{};
        error.Flags = LxssExecutionContextFlagsEnableContextualizedErrors | LxssExecutionContextFlagsEnableUserWarnings;
        error.WarningsPipe = 1;
        auto cleanup = wil::scope_exit([&]() {
            CoTaskMemFree(error.Message);
            CoTaskMemFree(error.Warnings);
        });

        // The missing-caller path reports GetLastError(), so supply a deterministic error.
        SetLastError(ERROR_INVALID_HANDLE);
        VERIFY_THROWS(wil::unique_handle{wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(error.WarningsPipe))}, wil::ResultException);

        {
            SetLastError(ERROR_INVALID_HANDLE);
            TestServiceExecutionContext context(&error);
            VERIFY_IS_TRUE(context.CollectUserWarning(L"first warning\n"));
            VERIFY_IS_TRUE(context.CollectUserWarning(L"second warning\n"));
            VERIFY_IS_NULL(error.Warnings);
        }

        VERIFY_IS_NOT_NULL(error.Warnings);
        VERIFY_ARE_EQUAL(std::wstring{L"first warning\nsecond warning\n"}, std::wstring{error.Warnings});
    }
};
