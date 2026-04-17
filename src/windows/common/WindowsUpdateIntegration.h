/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WindowsUpdateIntegration.h

Abstract:

    This file contains objects related to invoking the Windows Update Agent API.

--*/

#pragma once

namespace wsl::windows::common {
// Class factory for Windows Update Agent objects.
struct WindowsUpdateClassFactory
{
    virtual ~WindowsUpdateClassFactory() = default;

    virtual wil::com_ptr<IUpdateSession> CreateUpdateSession() const = 0;

    virtual wil::com_ptr<IUpdateCollection> CreateUpdateCollection() const = 0;
};

// Holds the context for performing a Windows Update Agent action.
struct WindowsUpdateContext
{
    // Create a context using the default class factory and WSL product.
    WindowsUpdateContext();

    // Create a context using the default class factory.
    WindowsUpdateContext(std::wstring product);

    // Create a context using the provided class factory.
    WindowsUpdateContext(std::unique_ptr<WindowsUpdateClassFactory> factory, std::wstring product);

    NON_COPYABLE(WindowsUpdateContext);
    DEFAULT_MOVABLE(WindowsUpdateContext);

    // Gets the appropriate product for the currently running WSL instance.
    static std::wstring WslProductIdentifier();

    // Ensures that the product is registered in with the Windows Update system.
    // This is required to use the system for initial installs.
    void EnsureProductRegistryEntry() const;

    // Searches for updates for the product.
    // Returns the number of updates found.
    size_t SearchForUpdates();

    // Gets the number of updates found by `SearchForUpdates`.
    size_t GetUpdateCount() const;

    // Downloads any updates that are not yet downloaded.
    // Calls the progress callback, if provided, with the overall download progress estimate.
    void DownloadUpdates(std::function<void(uint32_t)> progress = {}) const;

    // Installs any updates that were found.
    // Calls the progress callback, if provided, with the overall install progress estimate.
    void InstallUpdates(std::function<void(uint32_t)> progress = {}) const;

    static constexpr uint32_t DownloadProgressPercent = 70;
    static constexpr uint32_t InstallProgressPercent = 30;

    // Performs a complete update flow. This is a convenience method to remove the need to call and coordinate the individual actions.
    // When `forceInstall` is true, `EnsureProductRegistryEntry` is called.
    // Calls the progress callback, if provided, with the overall update progress estimate.
    //  Download and install phases are split according to the values defined above.
    void RunUpdateFlow(bool forceInstall = false, std::function<void(uint32_t)> progress = {});

private:
    using ActivityType = TraceLoggingActivity<g_hTraceLoggingProvider, MICROSOFT_KEYWORD_MEASURES>;

    std::unique_ptr<WindowsUpdateClassFactory> m_factory;
    std::wstring m_product;
    wil::com_ptr<IUpdateSession> m_session;
    wil::com_ptr<IUpdateSearcher> m_searcher;
    wil::com_ptr<IUpdateCollection> m_updates;
    std::unique_ptr<ActivityType> m_activity;
};
} // namespace wsl::windows::common
