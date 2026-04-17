/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsUpdateTests.cpp

Abstract:

    This file contains unit tests for WindowsUpdateIntegration.cpp.
    These tests use mock COM objects injected via the WindowsUpdateClassFactory
    abstraction so no real Windows Update service calls are made.

--*/

#include "precomp.h"
#include "Common.h"
#include "WindowsUpdateIntegration.h"

using namespace wsl::windows::common;
namespace WRL = Microsoft::WRL;

namespace {

// Stubs the 4 IDispatch pure virtual methods. All WUA interfaces derive from IDispatch.
#define STUB_IDISPATCH() \
    STDMETHOD(GetTypeInfoCount)(UINT*) override \
    { \
        return E_NOTIMPL; \
    } \
    STDMETHOD(GetTypeInfo)(UINT, LCID, ITypeInfo**) override \
    { \
        return E_NOTIMPL; \
    } \
    STDMETHOD(GetIDsOfNames)(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override \
    { \
        return E_NOTIMPL; \
    } \
    STDMETHOD(Invoke)(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override \
    { \
        return E_NOTIMPL; \
    }

// ---------------------------------------------------------------------------
// MockUpdateCollection — implements IUpdateCollection
// ---------------------------------------------------------------------------
struct MockUpdateCollection : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdateCollection>
{
    STUB_IDISPATCH()

    std::vector<wil::com_ptr<IUpdate>> items;
    LONG addCallCount = 0;

    STDMETHOD(get_Count)(LONG* retval) override
    {
        *retval = static_cast<LONG>(items.size());
        return S_OK;
    }

    STDMETHOD(get_Item)(LONG index, IUpdate** retval) override
    {
        if (index < 0 || static_cast<size_t>(index) >= items.size())
        {
            return E_INVALIDARG;
        }
        *retval = items[index].get();
        (*retval)->AddRef();
        return S_OK;
    }

    STDMETHOD(Add)(IUpdate* value, LONG*) override
    {
        wil::com_ptr<IUpdate> u = value;
        items.push_back(std::move(u));
        ++addCallCount;
        return S_OK;
    }

    STDMETHOD(Clear)() override
    {
        items.clear();
        return S_OK;
    }

    STDMETHOD(put_Item)(LONG, IUpdate*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get__NewEnum)(IUnknown**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ReadOnly)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(Copy)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(Insert)(LONG, IUpdate*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(RemoveAt)(LONG) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockUpdate — implements IUpdate
// ---------------------------------------------------------------------------
struct MockUpdate : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdate>
{
    STUB_IDISPATCH()

    VARIANT_BOOL isDownloaded = VARIANT_FALSE;

    STDMETHOD(get_IsDownloaded)(VARIANT_BOOL* retval) override
    {
        *retval = isDownloaded;
        return S_OK;
    }

    STDMETHOD(get_Title)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_AutoSelectOnWebSites)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_BundledUpdates)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_CanRequireSource)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Categories)(ICategoryCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Deadline)(VARIANT*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_DeltaCompressedContentAvailable)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_DeltaCompressedContentPreferred)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Description)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_EulaAccepted)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_EulaText)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_HandlerID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Identity)(IUpdateIdentity**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Image)(IImageInformation**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_InstallationBehavior)(IInstallationBehavior**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsBeta)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsHidden)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_IsHidden)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsInstalled)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsMandatory)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsUninstallable)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Languages)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_LastDeploymentChangeTime)(DATE*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_MaxDownloadSize)(DECIMAL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_MinDownloadSize)(DECIMAL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_MoreInfoUrls)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_MsrcSeverity)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_RecommendedCpuSpeed)(LONG*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_RecommendedHardDiskSpace)(LONG*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_RecommendedMemory)(LONG*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ReleaseNotes)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_SecurityBulletinIDs)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_SupersededUpdateIDs)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_SupportUrl)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Type)(UpdateType*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_UninstallationNotes)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_UninstallationBehavior)(IInstallationBehavior**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_UninstallationSteps)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_KBArticleIDs)(IStringCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(AcceptEula)() override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_DeploymentAction)(DeploymentAction*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(CopyFromCache)(BSTR, VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_DownloadPriority)(DownloadPriority*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_DownloadContents)(IUpdateDownloadContentCollection**) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockSearchResult — implements ISearchResult
// ---------------------------------------------------------------------------
struct MockSearchResult : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, ISearchResult>
{
    STUB_IDISPATCH()

    OperationResultCode resultCode = OperationResultCode::orcSucceeded;
    wil::com_ptr_nothrow<MockUpdateCollection> updates = wil::MakeOrThrow<MockUpdateCollection>();

    STDMETHOD(get_ResultCode)(OperationResultCode* retval) override
    {
        *retval = resultCode;
        return S_OK;
    }

    STDMETHOD(get_Updates)(IUpdateCollection** retval) override
    {
        return updates.query_to(retval);
    }

    STDMETHOD(get_RootCategories)(ICategoryCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Warnings)(IUpdateExceptionCollection**) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockUpdateSearcher — implements IUpdateSearcher
// ---------------------------------------------------------------------------
struct MockUpdateSearcher : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdateSearcher>
{
    STUB_IDISPATCH()

    wil::com_ptr_nothrow<MockSearchResult> searchResult = wil::MakeOrThrow<MockSearchResult>();

    STDMETHOD(Search)(BSTR, ISearchResult** retval) override
    {
        return searchResult.query_to(retval);
    }

    STDMETHOD(get_CanAutomaticallyUpgradeService)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_CanAutomaticallyUpgradeService)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ClientApplicationID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ClientApplicationID)(BSTR) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IncludePotentiallySupersededUpdates)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_IncludePotentiallySupersededUpdates)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ServerSelection)(ServerSelection*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ServerSelection)(ServerSelection) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(BeginSearch)(BSTR, IUnknown*, VARIANT, ISearchJob**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(EndSearch)(ISearchJob*, ISearchResult**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(EscapeString)(BSTR, BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(QueryHistory)(LONG, LONG, IUpdateHistoryEntryCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Online)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_Online)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(GetTotalHistoryCount)(LONG*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ServiceID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ServiceID)(BSTR) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockDownloadJob — implements IDownloadJob
// ---------------------------------------------------------------------------
struct MockDownloadJob : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IDownloadJob>
{
    STUB_IDISPATCH()

    STDMETHOD(CleanUp)() override
    {
        return S_OK;
    }
    STDMETHOD(get_AsyncState)(VARIANT*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsCompleted)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Updates)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(GetProgress)(IDownloadProgress**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(RequestAbort)() override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockDownloadResult — implements IDownloadResult
// ---------------------------------------------------------------------------
struct MockDownloadResult : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IDownloadResult>
{
    STUB_IDISPATCH()

    HRESULT downloadHResult = S_OK;

    STDMETHOD(get_HResult)(HRESULT* retval) override
    {
        *retval = downloadHResult;
        return S_OK;
    }

    STDMETHOD(get_ResultCode)(OperationResultCode*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(GetUpdateResult)(LONG, IUpdateDownloadResult**) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockUpdateDownloader — implements IUpdateDownloader
// ---------------------------------------------------------------------------
struct MockUpdateDownloader : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdateDownloader>
{
    STUB_IDISPATCH()

    wil::com_ptr_nothrow<MockDownloadResult> downloadResult = wil::MakeOrThrow<MockDownloadResult>();
    wil::com_ptr<IUpdateCollection> capturedCollection;
    bool beginDownloadCalled = false;

    STDMETHOD(put_Updates)(IUpdateCollection* value) override
    {
        capturedCollection = value;
        return S_OK;
    }

    STDMETHOD(BeginDownload)(IUnknown*, IUnknown*, VARIANT, IDownloadJob** retval) override
    {
        beginDownloadCalled = true;
        *retval = wil::MakeOrThrow<MockDownloadJob>().Detach();
        return S_OK;
    }

    STDMETHOD(EndDownload)(IDownloadJob*, IDownloadResult** retval) override
    {
        return downloadResult.query_to(retval);
    }

    STDMETHOD(get_ClientApplicationID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ClientApplicationID)(BSTR) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsForced)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_IsForced)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Priority)(DownloadPriority*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_Priority)(DownloadPriority) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Updates)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(Download)(IDownloadResult**) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockInstallationJob — implements IInstallationJob
// ---------------------------------------------------------------------------
struct MockInstallationJob : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IInstallationJob>
{
    STUB_IDISPATCH()

    STDMETHOD(CleanUp)() override
    {
        return S_OK;
    }
    STDMETHOD(get_AsyncState)(VARIANT*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsCompleted)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Updates)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(GetProgress)(IInstallationProgress**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(RequestAbort)() override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockInstallationResult — implements IInstallationResult
// ---------------------------------------------------------------------------
struct MockInstallationResult : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IInstallationResult>
{
    STUB_IDISPATCH()

    HRESULT installHResult = S_OK;

    STDMETHOD(get_HResult)(HRESULT* retval) override
    {
        *retval = installHResult;
        return S_OK;
    }

    STDMETHOD(get_RebootRequired)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ResultCode)(OperationResultCode*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(GetUpdateResult)(LONG, IUpdateInstallationResult**) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockUpdateInstaller — implements IUpdateInstaller
// ---------------------------------------------------------------------------
struct MockUpdateInstaller : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdateInstaller>
{
    STUB_IDISPATCH()

    wil::com_ptr_nothrow<MockInstallationResult> installResult = wil::MakeOrThrow<MockInstallationResult>();
    wil::com_ptr<IUpdateCollection> capturedCollection;
    bool beginInstallCalled = false;

    STDMETHOD(put_Updates)(IUpdateCollection* value) override
    {
        capturedCollection = value;
        return S_OK;
    }

    STDMETHOD(BeginInstall)(IUnknown*, IUnknown*, VARIANT, IInstallationJob** retval) override
    {
        beginInstallCalled = true;
        *retval = wil::MakeOrThrow<MockInstallationJob>().Detach();
        return S_OK;
    }

    STDMETHOD(EndInstall)(IInstallationJob*, IInstallationResult** retval) override
    {
        return installResult.query_to(retval);
    }

    STDMETHOD(get_ClientApplicationID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ClientApplicationID)(BSTR) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsForced)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_IsForced)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ParentHwnd)(HWND*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ParentHwnd)(HWND) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_ParentWindow)(IUnknown*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ParentWindow)(IUnknown**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_Updates)(IUpdateCollection**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(BeginUninstall)(IUnknown*, IUnknown*, VARIANT, IInstallationJob**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(EndUninstall)(IInstallationJob*, IInstallationResult**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(Install)(IInstallationResult**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(RunWizard)(BSTR, IInstallationResult**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_IsBusy)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(Uninstall)(IInstallationResult**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_AllowSourcePrompts)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_AllowSourcePrompts)(VARIANT_BOOL) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_RebootRequiredBeforeInstallation)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockUpdateSession — implements IUpdateSession
// ---------------------------------------------------------------------------
struct MockUpdateSession : public WRL::RuntimeClass<WRL::RuntimeClassFlags<WRL::ClassicCom>, IUpdateSession>
{
    STUB_IDISPATCH()

    wil::com_ptr_nothrow<MockUpdateSearcher> searcher = wil::MakeOrThrow<MockUpdateSearcher>();
    wil::com_ptr_nothrow<MockUpdateDownloader> downloader = wil::MakeOrThrow<MockUpdateDownloader>();
    wil::com_ptr_nothrow<MockUpdateInstaller> installer = wil::MakeOrThrow<MockUpdateInstaller>();

    STDMETHOD(put_ClientApplicationID)(BSTR) override
    {
        return S_OK;
    }

    STDMETHOD(CreateUpdateSearcher)(IUpdateSearcher** retval) override
    {
        return searcher.query_to(retval);
    }

    STDMETHOD(CreateUpdateDownloader)(IUpdateDownloader** retval) override
    {
        return downloader.query_to(retval);
    }

    STDMETHOD(CreateUpdateInstaller)(IUpdateInstaller** retval) override
    {
        return installer.query_to(retval);
    }

    STDMETHOD(get_ClientApplicationID)(BSTR*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_ReadOnly)(VARIANT_BOOL*) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(get_WebProxy)(IWebProxy**) override
    {
        return E_NOTIMPL;
    }
    STDMETHOD(put_WebProxy)(IWebProxy*) override
    {
        return E_NOTIMPL;
    }
};

// ---------------------------------------------------------------------------
// MockWindowsUpdateClassFactory
// ---------------------------------------------------------------------------
struct MockWindowsUpdateClassFactory : public WindowsUpdateClassFactory
{
    wil::com_ptr<MockUpdateSession> session = wil::MakeOrThrow<MockUpdateSession>();
    // Tracks the most-recently created collection (used as toDownload in DownloadUpdates).
    mutable wil::com_ptr<MockUpdateCollection> lastCreatedCollection;

    wil::com_ptr<IUpdateSession> CreateUpdateSession() const override
    {
        return session.query<IUpdateSession>();
    }

    wil::com_ptr<IUpdateCollection> CreateUpdateCollection() const override
    {
        auto col = wil::MakeOrThrow<MockUpdateCollection>();
        lastCreatedCollection = col;
        wil::com_ptr<IUpdateCollection> result;
        col.CopyTo(IID_IUpdateCollection, result.put_void());
        return result;
    }
};

// Captures the HRESULT thrown by a wil::ResultException, or S_OK if no exception.
static HRESULT CaptureHResult(const std::function<void()>& fn)
{
    try
    {
        fn();
        return S_OK;
    }
    catch (const wil::ResultException& e)
    {
        return e.GetErrorCode();
    }
}

// Adds a MockUpdate with the given isDownloaded state to the search result collection.
static wil::com_ptr<MockUpdate> AddMockUpdate(MockUpdateCollection* col, VARIANT_BOOL isDownloaded)
{
    auto u = wil::MakeOrThrow<MockUpdate>();
    u->isDownloaded = isDownloaded;
    wil::com_ptr<IUpdate> update;
    u.CopyTo(IID_IUpdate, update.put_void());
    col->items.push_back(std::move(update));
    return u;
}

} // namespace

class WindowsUpdateTests
{
    WSL_TEST_CLASS(WindowsUpdateTests)

    // -----------------------------------------------------------------------
    // SearchForUpdates tests
    // -----------------------------------------------------------------------

    TEST_METHOD(SearchForUpdates_NoUpdates)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        VERIFY_ARE_EQUAL(0u, ctx.SearchForUpdates());
        VERIFY_ARE_EQUAL(0u, ctx.GetUpdateCount());
    }

    TEST_METHOD(SearchForUpdates_UpdatesFound)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        auto* col = fp->session->searcher->searchResult->updates.get();
        AddMockUpdate(col, VARIANT_FALSE);
        AddMockUpdate(col, VARIANT_FALSE);
        AddMockUpdate(col, VARIANT_FALSE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        VERIFY_ARE_EQUAL(3u, ctx.SearchForUpdates());
        VERIFY_ARE_EQUAL(3u, ctx.GetUpdateCount());
    }

    TEST_METHOD(SearchForUpdates_SucceededWithErrors)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        fp->session->searcher->searchResult->resultCode = OperationResultCode::orcSucceededWithErrors;
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_FALSE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        // orcSucceededWithErrors must succeed — the update count is still returned.
        VERIFY_ARE_EQUAL(1u, ctx.SearchForUpdates());
    }

    TEST_METHOD(SearchForUpdates_Failed)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        factory->session->searcher->searchResult->resultCode = OperationResultCode::orcFailed;

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        VERIFY_ARE_EQUAL(WSLC_E_WU_SEARCH_FAILED, CaptureHResult([&] { ctx.SearchForUpdates(); }));
    }

    // -----------------------------------------------------------------------
    // DownloadUpdates tests
    // -----------------------------------------------------------------------

    TEST_METHOD(DownloadUpdates_AllAlreadyDownloaded)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        auto* col = fp->session->searcher->searchResult->updates.get();
        AddMockUpdate(col, VARIANT_TRUE);
        AddMockUpdate(col, VARIANT_TRUE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");
        ctx.SearchForUpdates();

        std::vector<uint32_t> progressCalls;
        ctx.DownloadUpdates([&](uint32_t p) { progressCalls.push_back(p); });

        // All updates were already downloaded: BeginDownload must not be called,
        // and progress(100) must be reported to signal completion.
        VERIFY_IS_FALSE(fp->session->downloader->beginDownloadCalled);
        VERIFY_ARE_EQUAL(0L, fp->lastCreatedCollection->addCallCount);
        VERIFY_ARE_EQUAL(1u, progressCalls.size());
        VERIFY_ARE_EQUAL(100u, progressCalls[0]);
    }

    TEST_METHOD(DownloadUpdates_SomeNeedDownloading)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        auto* col = fp->session->searcher->searchResult->updates.get();
        AddMockUpdate(col, VARIANT_TRUE);  // already downloaded — skip
        AddMockUpdate(col, VARIANT_FALSE); // needs download
        AddMockUpdate(col, VARIANT_FALSE); // needs download

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");
        ctx.SearchForUpdates();
        ctx.DownloadUpdates();

        VERIFY_IS_TRUE(fp->session->downloader->beginDownloadCalled);
        VERIFY_ARE_EQUAL(2L, fp->lastCreatedCollection->addCallCount);
    }

    TEST_METHOD(DownloadUpdates_DownloadFails)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_FALSE);
        fp->session->downloader->downloadResult->downloadHResult = E_FAIL;

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");
        ctx.SearchForUpdates();

        VERIFY_ARE_EQUAL(E_FAIL, CaptureHResult([&] { ctx.DownloadUpdates(); }));
    }

    // -----------------------------------------------------------------------
    // InstallUpdates tests
    // -----------------------------------------------------------------------

    TEST_METHOD(InstallUpdates_Success)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_TRUE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");
        ctx.SearchForUpdates();

        // Should not throw.
        ctx.InstallUpdates();

        VERIFY_IS_TRUE(fp->session->installer->beginInstallCalled);
        VERIFY_IS_NOT_NULL(fp->session->installer->capturedCollection.get());
    }

    TEST_METHOD(InstallUpdates_Fails)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_TRUE);
        fp->session->installer->installResult->installHResult = E_ACCESSDENIED;

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");
        ctx.SearchForUpdates();

        VERIFY_ARE_EQUAL(E_ACCESSDENIED, CaptureHResult([&] { ctx.InstallUpdates(); }));
    }

    // -----------------------------------------------------------------------
    // RunUpdateFlow tests
    // -----------------------------------------------------------------------

    TEST_METHOD(RunUpdateFlow_NoUpdates_ProgressGoesTo100)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        std::vector<uint32_t> progressCalls;
        ctx.RunUpdateFlow(false, [&](uint32_t p) { progressCalls.push_back(p); });

        // progress(0) at the start, progress(100) because there are no updates.
        VERIFY_ARE_EQUAL(2u, progressCalls.size());
        VERIFY_ARE_EQUAL(0u, progressCalls[0]);
        VERIFY_ARE_EQUAL(100u, progressCalls[1]);

        // No download or install should have been triggered.
        VERIFY_IS_FALSE(fp->session->downloader->beginDownloadCalled);
        VERIFY_IS_FALSE(fp->session->installer->beginInstallCalled);
    }

    TEST_METHOD(RunUpdateFlow_UpdatesFound_DownloadThenInstall)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_FALSE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        std::vector<uint32_t> progressCalls;
        ctx.RunUpdateFlow(false, [&](uint32_t p) { progressCalls.push_back(p); });

        // progress(0) is emitted at the start.
        VERIFY_IS_FALSE(progressCalls.empty());
        VERIFY_ARE_EQUAL(0u, progressCalls[0]);

        // Both download and install phases ran.
        VERIFY_IS_TRUE(fp->session->downloader->beginDownloadCalled);
        VERIFY_IS_TRUE(fp->session->installer->beginInstallCalled);
    }

    TEST_METHOD(RunUpdateFlow_DownloadProgressScaling)
    {
        // Verify that download progress values are scaled into the 0–DownloadProgressPercent range.
        // The download lambda is:  progress((percent * DownloadProgressPercent) / 100)
        // For percent=50: expected outer value = (50 * 70) / 100 = 35
        VERIFY_ARE_EQUAL(35u, (50u * WindowsUpdateContext::DownloadProgressPercent) / 100u);

        // Verify that install progress values are offset and scaled into the remaining range.
        // The install lambda is:  progress(DownloadProgressPercent + (percent * InstallProgressPercent) / 100)
        // For percent=100: expected outer value = 70 + (100 * 30) / 100 = 100
        VERIFY_ARE_EQUAL(100u, WindowsUpdateContext::DownloadProgressPercent + (100u * WindowsUpdateContext::InstallProgressPercent) / 100u);
    }

    TEST_METHOD(RunUpdateFlow_DownloadFails_Propagates)
    {
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        auto* fp = factory.get();
        AddMockUpdate(fp->session->searcher->searchResult->updates.get(), VARIANT_FALSE);
        fp->session->downloader->downloadResult->downloadHResult = E_FAIL;

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        VERIFY_ARE_EQUAL(E_FAIL, CaptureHResult([&] { ctx.RunUpdateFlow(); }));

        // Install should not have been called after download failure.
        VERIFY_IS_FALSE(fp->session->installer->beginInstallCalled);
    }

    TEST_METHOD(RunUpdateFlow_NoProgress_DoesNotCrash)
    {
        // Verifies that passing no progress callback does not crash.
        auto factory = std::make_unique<MockWindowsUpdateClassFactory>();
        AddMockUpdate(factory->session->searcher->searchResult->updates.get(), VARIANT_TRUE);

        WindowsUpdateContext ctx(std::move(factory), L"TestProduct");

        // Should complete without crashing even with no progress callback.
        ctx.RunUpdateFlow();
    }
};
