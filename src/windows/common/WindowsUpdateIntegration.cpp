/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsUpdateIntegration.cpp

Abstract:

    This file contains objects related to invoking the Windows Update Agent API.

--*/

#include "precomp.h"
#include "WindowsUpdateIntegration.h"

namespace wsl::windows::common {

namespace anon {
    struct DefaultWindowsUpdateClassFactory : public WindowsUpdateClassFactory
    {
        wil::com_ptr<IUpdateSession> CreateUpdateSession() const override
        {
            wil::com_ptr<IUpdateSession> result;
            THROW_IF_FAILED(CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER, IID_IUpdateSession, (void**)&result));
            return result;
        }

        wil::com_ptr<IUpdateCollection> CreateUpdateCollection() const override
        {
            wil::com_ptr<IUpdateCollection> result;
            THROW_IF_FAILED(CoCreateInstance(CLSID_UpdateCollection, nullptr, CLSCTX_INPROC_SERVER, IID_IUpdateCollection, (void**)&result));
            return result;
        }
    };

    struct DownloadProgressChangedCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDownloadProgressChangedCallback>
    {
        DownloadProgressChangedCallback(std::function<void(uint32_t)> progress) : m_progress(progress)
        {
        }

        IFACEMETHOD(Invoke)(IDownloadJob*, IDownloadProgressChangedCallbackArgs* callbackArgs) override
        {
            wil::com_ptr<IDownloadProgress> progress;
            RETURN_IF_FAILED(callbackArgs->get_Progress(&progress));

            LONG percent{};
            RETURN_IF_FAILED(progress->get_PercentComplete(&percent));

            m_progress(static_cast<uint32_t>(percent));
            return S_OK;
        }

    private:
        std::function<void(uint32_t)> m_progress;
    };

    struct DownloadCompletedCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDownloadCompletedCallback>
    {
        DownloadCompletedCallback()
        {
        }

        IFACEMETHOD(Invoke)(IDownloadJob*, IDownloadCompletedCallbackArgs*) override
        {
            m_completed.SetEvent();
            return S_OK;
        }

        void Wait()
        {
            m_completed.wait();
        }

    private:
        wil::slim_event_manual_reset m_completed;
    };

    struct InstallationProgressChangedCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IInstallationProgressChangedCallback>
    {
        InstallationProgressChangedCallback(std::function<void(uint32_t)> progress) : m_progress(progress)
        {
        }

        IFACEMETHOD(Invoke)(IInstallationJob*, IInstallationProgressChangedCallbackArgs* callbackArgs) override
        {
            wil::com_ptr<IInstallationProgress> progress;
            RETURN_IF_FAILED(callbackArgs->get_Progress(&progress));

            LONG percent{};
            RETURN_IF_FAILED(progress->get_PercentComplete(&percent));

            m_progress(static_cast<uint32_t>(percent));
            return S_OK;
        }

    private:
        std::function<void(uint32_t)> m_progress;
    };

    struct InstallationCompletedCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IInstallationCompletedCallback>
    {
        InstallationCompletedCallback()
        {
        }

        IFACEMETHOD(Invoke)(IInstallationJob*, IInstallationCompletedCallbackArgs*) override
        {
            m_completed.SetEvent();
            return S_OK;
        }

        void Wait()
        {
            m_completed.wait();
        }

    private:
        wil::slim_event_manual_reset m_completed;
    };
} // namespace anon

WindowsUpdateContext::WindowsUpdateContext() :
    WindowsUpdateContext(std::make_unique<anon::DefaultWindowsUpdateClassFactory>(), WslProductIdentifier())
{
}

WindowsUpdateContext::WindowsUpdateContext(std::wstring product) :
    WindowsUpdateContext(std::make_unique<anon::DefaultWindowsUpdateClassFactory>(), std::move(product))
{
}

WindowsUpdateContext::WindowsUpdateContext(std::unique_ptr<WindowsUpdateClassFactory> factory, std::wstring product) :
    m_factory(std::move(factory)), m_product(std::move(product))
{
    m_session = m_factory->CreateUpdateSession();

    auto applicationID = wil::make_bstr(L"Windows Subsystem for Linux");
    THROW_IF_FAILED(m_session->put_ClientApplicationID(applicationID.get()));

    m_activity = std::make_unique<ActivityType>();
    TraceLoggingWriteStart(
        *m_activity,
        "WindowsUpdateContext",
        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
        TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"),
        TraceLoggingWideString(m_product.c_str(), "product"));
}

std::wstring WindowsUpdateContext::WslProductIdentifier()
{
    return STRING_TO_WIDE_STRING(DCAT_PRODUCT_NAME);
}

void WindowsUpdateContext::EnsureProductRegistryEntry() const
{
    wsl::windows::common::helpers::RegisterWithDcat(false);
}

size_t WindowsUpdateContext::SearchForUpdates()
{
    TraceLoggingWriteTagged(
        *m_activity, "SearchForUpdates", TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    THROW_IF_FAILED(m_session->CreateUpdateSearcher(&m_searcher));

    std::wstring queryString = std::format(L"Product='{}'", m_product);
    auto queryBSTR = wil::make_bstr(queryString.c_str());

    wil::com_ptr<ISearchResult> searchResult;
    THROW_IF_FAILED(m_searcher->Search(queryBSTR.get(), &searchResult));

    OperationResultCode resultCode{};
    THROW_IF_FAILED(searchResult->get_ResultCode(&resultCode));

    THROW_HR_IF(WSLC_E_WU_SEARCH_FAILED, resultCode != OperationResultCode::orcSucceeded && resultCode != OperationResultCode::orcSucceededWithErrors);

    if (resultCode == OperationResultCode::orcSucceededWithErrors)
    {
        // TODO: Consider logging from the warnings
    }

    THROW_IF_FAILED(searchResult->get_Updates(&m_updates));
    return GetUpdateCount();
}

size_t WindowsUpdateContext::GetUpdateCount() const
{
    LONG result{};
    if (m_updates)
    {
        THROW_IF_FAILED(m_updates->get_Count(&result));
    }
    return static_cast<size_t>(result);
}

void WindowsUpdateContext::DownloadUpdates(std::function<void(uint32_t)> progress) const
{
    TraceLoggingWriteTagged(
        *m_activity, "DownloadUpdates", TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    // Collect all of the updates that are not currently downloaded
    wil::com_ptr<IUpdateCollection> toDownload = m_factory->CreateUpdateCollection();

    for (size_t i = 0, count = GetUpdateCount(); i < count; ++i)
    {
        wil::com_ptr<IUpdate> update;
        THROW_IF_FAILED(m_updates->get_Item(static_cast<LONG>(i), &update));
        VARIANT_BOOL downloaded = VARIANT_FALSE;
        THROW_IF_FAILED(update->get_IsDownloaded(&downloaded));
        if (downloaded == VARIANT_FALSE)
        {
            THROW_IF_FAILED(toDownload->Add(update.get(), nullptr));
        }
    }

    // All updates are already downloaded — nothing to do.
    LONG toDownloadCount{};
    THROW_IF_FAILED(toDownload->get_Count(&toDownloadCount));
    if (toDownloadCount == 0)
    {
        if (progress)
        {
            progress(100);
        }
        return;
    }

    wil::com_ptr<IUpdateDownloader> updateDownloader;
    THROW_IF_FAILED(m_session->CreateUpdateDownloader(&updateDownloader));

    THROW_IF_FAILED(updateDownloader->put_Updates(toDownload.get()));

    Microsoft::WRL::ComPtr<anon::DownloadProgressChangedCallback> downloadProgress;
    if (progress)
    {
        downloadProgress = wil::MakeOrThrow<anon::DownloadProgressChangedCallback>(progress);
    }
    auto downloadCompleted = wil::MakeOrThrow<anon::DownloadCompletedCallback>();
    wil::com_ptr<IDownloadJob> downloadJob;

    THROW_IF_FAILED(updateDownloader->BeginDownload(downloadProgress.Get(), downloadCompleted.Get(), VARIANT{}, &downloadJob));
    downloadCompleted->Wait();
    THROW_IF_FAILED(downloadJob->CleanUp());

    wil::com_ptr<IDownloadResult> result;
    THROW_IF_FAILED(updateDownloader->EndDownload(downloadJob.get(), &result));

    HRESULT downloadHResult{};
    THROW_IF_FAILED(result->get_HResult(&downloadHResult));
    THROW_IF_FAILED(downloadHResult);
}

void WindowsUpdateContext::InstallUpdates(std::function<void(uint32_t)> progress) const
{
    TraceLoggingWriteTagged(
        *m_activity, "InstallUpdates", TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    wil::com_ptr<IUpdateInstaller> updateInstaller;
    THROW_IF_FAILED(m_session->CreateUpdateInstaller(&updateInstaller));

    THROW_IF_FAILED(updateInstaller->put_Updates(m_updates.get()));

    Microsoft::WRL::ComPtr<anon::InstallationProgressChangedCallback> installationProgress;
    if (progress)
    {
        installationProgress = wil::MakeOrThrow<anon::InstallationProgressChangedCallback>(progress);
    }
    auto installationCompleted = wil::MakeOrThrow<anon::InstallationCompletedCallback>();
    wil::com_ptr<IInstallationJob> installationJob;

    THROW_IF_FAILED(updateInstaller->BeginInstall(installationProgress.Get(), installationCompleted.Get(), VARIANT{}, &installationJob));
    installationCompleted->Wait();
    THROW_IF_FAILED(installationJob->CleanUp());

    wil::com_ptr<IInstallationResult> result;
    THROW_IF_FAILED(updateInstaller->EndInstall(installationJob.get(), &result));

    HRESULT installationHResult{};
    THROW_IF_FAILED(result->get_HResult(&installationHResult));
    THROW_IF_FAILED(installationHResult);
}

void WindowsUpdateContext::RunUpdateFlow(bool forceInstall, std::function<void(uint32_t)> progress)
{
    TraceLoggingWriteTagged(
        *m_activity, "RunUpdateFlow", TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

    static_assert(
        DownloadProgressPercent + InstallProgressPercent == 100, "Download and Install progress values must add up to 100.");

    if (progress)
    {
        progress(0);
    }

    if (forceInstall)
    {
        EnsureProductRegistryEntry();
    }

    size_t updateCount = SearchForUpdates();
    if (!updateCount)
    {
        if (progress)
        {
            progress(100);
        }
        return;
    }

    std::function<void(uint32_t)> downloadProgress;
    if (progress)
    {
        downloadProgress = [&](uint32_t percent) { progress((percent * DownloadProgressPercent) / 100); };
    }
    DownloadUpdates(downloadProgress);

    std::function<void(uint32_t)> installProgress;
    if (progress)
    {
        installProgress = [&](uint32_t percent) { progress(DownloadProgressPercent + ((percent * InstallProgressPercent) / 100)); };
    }
    InstallUpdates(installProgress);
}
} // namespace wsl::windows::common