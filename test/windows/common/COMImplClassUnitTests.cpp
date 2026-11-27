// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "COMImplClass.h"

#include <future>

using wsl::windows::service::wslc::COMImplClass;

namespace {

struct TestImpl
{
    ~TestImpl()
    {
        if (OnDestroy)
        {
            OnDestroy();
        }
    }

    void Call()
    {
        if (OnCall)
        {
            OnCall();
        }
    }

    std::function<void()> OnCall;
    std::function<void()> OnDestroy;
};

class TestWrapper : public COMImplClass<TestImpl, std::weak_ptr<TestImpl>>
{
public:
    HRESULT Call()
    {
        return CallImpl(&TestImpl::Call);
    }
};

} // namespace

namespace COMImplClassUnitTests {
class COMImplClassUnitTests
{
    WSLC_TEST_CLASS(COMImplClassUnitTests)

    // A caller that ends up owning the last impl reference must unregister before the impl is destroyed,
    // so that an impl destructor waiting on a concurrent Disconnect() can make progress.
    TEST_METHOD(LastReferenceReleasedAfterCallerUnregisters)
    {
        TestWrapper wrapper;
        std::future<void> disconnect;
        bool disconnectCompleted = false;

        auto impl = std::make_shared<TestImpl>();
        wrapper.Initialize(impl);

        impl->OnCall = [&]() { impl.reset(); };
        impl->OnDestroy = [&]() {
            disconnect = std::async(std::launch::async, [&]() { wrapper.Disconnect(); });
            disconnectCompleted = disconnect.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
        };

        VERIFY_ARE_EQUAL(S_OK, wrapper.Call());
        VERIFY_IS_TRUE(impl == nullptr);
        VERIFY_IS_TRUE(disconnectCompleted);
        VERIFY_ARE_EQUAL(RPC_E_DISCONNECTED, wrapper.Call());
    }
};
} // namespace COMImplClassUnitTests
