// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "RuntimeClass.h"

namespace RuntimeClassConstructorGuardUnitTests {

namespace {

    class TestModule : public Microsoft::WRL::Details::ModuleBase
    {
    public:
        STDMETHOD_(ULONG, IncrementObjectCount)() override
        {
            return ++m_objectCount;
        }

        STDMETHOD_(ULONG, DecrementObjectCount)() override
        {
            return --m_objectCount;
        }

        STDMETHOD_(ULONG, GetObjectCount)() const override
        {
            return m_objectCount;
        }

        STDMETHOD(RegisterWinRTObject)(const wchar_t*, const wchar_t**, RO_REGISTRATION_COOKIE*, unsigned int) override
        {
            return E_NOTIMPL;
        }

        STDMETHOD(UnregisterWinRTObject)(const wchar_t*, RO_REGISTRATION_COOKIE) override
        {
            return E_NOTIMPL;
        }

        STDMETHOD(RegisterCOMObject)(const wchar_t*, IID*, IClassFactory**, DWORD*, unsigned int) override
        {
            return E_NOTIMPL;
        }

        STDMETHOD(UnregisterCOMObject)(const wchar_t*, DWORD*, unsigned int) override
        {
            return E_NOTIMPL;
        }

    private:
        std::atomic<ULONG> m_objectCount{};
    };

    enum class FailurePoint
    {
        None,
        Base,
        Member,
        Body
    };

    class LifetimeProbe
    {
    public:
        LifetimeProbe(bool Fail, ULONG& CountAtDestruction) : m_countAtDestruction(CountAtDestruction)
        {
            THROW_HR_IF(E_ABORT, Fail);
        }

        ~LifetimeProbe()
        {
            m_countAtDestruction = Microsoft::WRL::GetModuleBase()->GetObjectCount();
        }

    private:
        ULONG& m_countAtDestruction;
    };

    template <unsigned int Flags>
    class GuardedObject : public wsl::windows::common::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Flags>, IUnknown>, public LifetimeProbe
    {
    public:
        GuardedObject(FailurePoint Failure, ULONG& BaseCount, ULONG& MemberCount) :
            LifetimeProbe(Failure == FailurePoint::Base, BaseCount), m_member(Failure == FailurePoint::Member, MemberCount)
        {
            if constexpr (Flags == Microsoft::WRL::WinRtClassicComMix)
            {
                Microsoft::WRL::ComPtr<IWeakReference> weak;
                THROW_IF_FAILED(this->GetWeakReference(&weak));
            }

            // Nonfinal Release during construction must not disable rollback.
            Microsoft::WRL::ComPtr<IUnknown> self;
            THROW_IF_FAILED(this->QueryInterface(IID_PPV_ARGS(&self)));
            self.Reset();

            THROW_HR_IF(E_ABORT, Failure == FailurePoint::Body);
        }

    private:
        LifetimeProbe m_member;
    };

} // namespace

class RuntimeClassConstructorGuardUnitTests
{
    WSL_TEST_CLASS(RuntimeClassConstructorGuardUnitTests)

    TEST_METHOD(ConstructorFailureReleasesModuleAfterCleanup)
    {
        VerifyConstructorFailure<Microsoft::WRL::ClassicCom>();
        VerifyConstructorFailure<Microsoft::WRL::WinRtClassicComMix>();
    }

    TEST_METHOD(SuccessfulConstructionLeavesModuleReleaseToWrl)
    {
        VerifyFinalRelease<Microsoft::WRL::ClassicCom>(false);
        VerifyFinalRelease<Microsoft::WRL::WinRtClassicComMix>(false);
    }

    TEST_METHOD(FinalReleaseDuringExceptionUnwindingDoesNotDoubleDecrement)
    {
        VerifyFinalRelease<Microsoft::WRL::ClassicCom>(true);
        VerifyFinalRelease<Microsoft::WRL::WinRtClassicComMix>(true);
    }

    template <unsigned int Flags>
    static void VerifyConstructorFailure()
    {
        VERIFY_IS_NULL(Microsoft::WRL::GetModuleBase());
        TestModule module;

        for (const auto failure : {FailurePoint::Base, FailurePoint::Member, FailurePoint::Body})
        {
            ULONG baseCount = MAXULONG;
            ULONG memberCount = MAXULONG;
            VERIFY_THROWS_SPECIFIC(
                Microsoft::WRL::Make<GuardedObject<Flags>>(failure, baseCount, memberCount),
                wil::ResultException,
                [](const wil::ResultException& error) { return error.GetErrorCode() == E_ABORT; });

            VERIFY_ARE_EQUAL(0UL, module.GetObjectCount());
            const ULONG expectedCount = Flags == Microsoft::WRL::WinRtClassicComMix && failure == FailurePoint::Body ? 2 : 1;
            if (failure != FailurePoint::Base)
            {
                VERIFY_ARE_EQUAL(expectedCount, baseCount);
            }

            if (failure == FailurePoint::Body)
            {
                VERIFY_ARE_EQUAL(expectedCount, memberCount);
            }
        }
    }

    template <unsigned int Flags>
    static void VerifyFinalRelease(bool Unwind)
    {
        VERIFY_IS_NULL(Microsoft::WRL::GetModuleBase());
        TestModule module;
        ULONG baseCount = MAXULONG;
        ULONG memberCount = MAXULONG;

        constexpr ULONG expectedCount = Flags == Microsoft::WRL::WinRtClassicComMix ? 2 : 1;
        auto createAndRelease = [&]() {
            auto object = Microsoft::WRL::Make<GuardedObject<Flags>>(FailurePoint::None, baseCount, memberCount);
            VERIFY_IS_NOT_NULL(object.Get());
            VERIFY_ARE_EQUAL(expectedCount, module.GetObjectCount());
            THROW_HR_IF(E_ABORT, Unwind);
        };
        if (Unwind)
        {
            VERIFY_THROWS_SPECIFIC(createAndRelease(), wil::ResultException, [](const wil::ResultException& error) {
                return error.GetErrorCode() == E_ABORT;
            });
        }
        else
        {
            createAndRelease();
        }

        VERIFY_ARE_EQUAL(expectedCount, memberCount);
        VERIFY_ARE_EQUAL(expectedCount, baseCount);
        VERIFY_ARE_EQUAL(0UL, module.GetObjectCount());
    }
};

} // namespace RuntimeClassConstructorGuardUnitTests
