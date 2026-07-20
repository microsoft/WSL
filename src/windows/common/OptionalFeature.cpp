// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OptionalFeature.h"

namespace {
using DismSession = unsigned int;

constexpr DismSession c_dismSessionDefault = 0;
constexpr auto c_dismOnlineImage = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";
std::mutex g_dismLock;

enum class DismLogLevel
{
    Errors = 0,
    ErrorsWarnings,
    ErrorsWarningsInfo
};

enum class DismPackageIdentifier
{
    None = 0
};

#pragma pack(push, 1)
struct DismFeatureInfo
{
    PCWSTR FeatureName;
    wsl::windows::common::optionalfeature::details::DismFeatureState FeatureState;
};
#pragma pack(pop)

using DismInitializeFunction = HRESULT WINAPI(DismLogLevel, PCWSTR, PCWSTR);
using DismShutdownFunction = HRESULT WINAPI();
using DismOpenSessionFunction = HRESULT WINAPI(PCWSTR, PCWSTR, PCWSTR, DismSession*);
using DismCloseSessionFunction = HRESULT WINAPI(DismSession);
using DismGetFeatureInfoFunction = HRESULT WINAPI(DismSession, PCWSTR, PCWSTR, DismPackageIdentifier, DismFeatureInfo**);
using DismDeleteFunction = HRESULT WINAPI(void*);

wil::shared_hmodule LoadDismApi()
{
    wil::shared_hmodule module{LoadLibraryExW(L"dismapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    THROW_LAST_ERROR_IF(!module);
    return module;
}
} // namespace

namespace wsl::windows::common::optionalfeature {
State details::MapDismFeatureState(DismFeatureState state)
{
    switch (state)
    {
    case DismFeatureState::NotPresent:
    case DismFeatureState::Staged:
    case DismFeatureState::Removed:
        return State::Disabled;

    case DismFeatureState::UninstallPending:
        return State::DisablePending;

    case DismFeatureState::Installed:
        return State::Enabled;

    case DismFeatureState::InstallPending:
        return State::EnablePending;

    default:
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected DISM feature state: %u", static_cast<unsigned int>(state));
    }
}

class Query::Impl
{
public:
    Impl() :
        m_dismLock{g_dismLock},
        m_module{LoadDismApi()},
        m_initialize{m_module, "DismInitialize"},
        m_shutdown{m_module, "DismShutdown"},
        m_openSession{m_module, "DismOpenSession"},
        m_closeSession{m_module, "DismCloseSession"},
        m_getFeatureInfo{m_module, "DismGetFeatureInfo"},
        m_delete{m_module, "DismDelete"}
    {
        THROW_IF_FAILED(m_initialize(DismLogLevel::ErrorsWarnings, nullptr, nullptr));
        m_initialized = true;

        auto shutdownOnFailure = wil::scope_exit([&]() { LOG_IF_FAILED(m_shutdown()); });
        THROW_IF_FAILED(m_openSession(c_dismOnlineImage, nullptr, nullptr, &m_session));
        shutdownOnFailure.release();
    }

    ~Impl()
    {
        if (m_session != c_dismSessionDefault)
        {
            LOG_IF_FAILED(m_closeSession(m_session));
        }

        if (m_initialized)
        {
            LOG_IF_FAILED(m_shutdown());
        }
    }

    State GetState(std::wstring_view featureName)
    {
        THROW_HR_IF(E_INVALIDARG, featureName.empty());

        const std::wstring nullTerminatedName{featureName};
        DismFeatureInfo* featureInfo{};
        THROW_IF_FAILED(m_getFeatureInfo(m_session, nullTerminatedName.c_str(), nullptr, DismPackageIdentifier::None, &featureInfo));
        THROW_HR_IF(E_UNEXPECTED, featureInfo == nullptr);

        auto deleteFeatureInfo = wil::scope_exit([&]() { LOG_IF_FAILED(m_delete(featureInfo)); });
        return details::MapDismFeatureState(featureInfo->FeatureState);
    }

private:
    std::unique_lock<std::mutex> m_dismLock;
    wil::shared_hmodule m_module;
    LxssDynamicFunction<DismInitializeFunction> m_initialize;
    LxssDynamicFunction<DismShutdownFunction> m_shutdown;
    LxssDynamicFunction<DismOpenSessionFunction> m_openSession;
    LxssDynamicFunction<DismCloseSessionFunction> m_closeSession;
    LxssDynamicFunction<DismGetFeatureInfoFunction> m_getFeatureInfo;
    LxssDynamicFunction<DismDeleteFunction> m_delete;
    DismSession m_session{c_dismSessionDefault};
    bool m_initialized{};
};

Query::Query() : m_impl{std::make_unique<Impl>()}
{
}

Query::~Query() = default;

State Query::GetState(std::wstring_view featureName)
{
    return m_impl->GetState(featureName);
}
} // namespace wsl::windows::common::optionalfeature
