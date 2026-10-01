/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionTests.cpp

Abstract:

    This file contains test cases for the WSLC session API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCSessionTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCSessionTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(GetVersion)
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        WSLCVersion version{};

        VERIFY_SUCCEEDED(sessionManager->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    WSLC_TEST_METHOD(IsClientVersionSupported)
    {
        wil::com_ptr<IWSLCCompatSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        BOOL isSupported = FALSE;

        // The current version should always be supported.
        const WSLCCompatVersion currentVersion{WSL_PACKAGE_VERSION_MAJOR, WSL_PACKAGE_VERSION_MINOR, WSL_PACKAGE_VERSION_REVISION};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&currentVersion, &isSupported));
        VERIFY_IS_TRUE(isSupported);

        // A very old version should not be supported.
        const WSLCCompatVersion oldVersion{1, 0, 0};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&oldVersion, &isSupported));
        VERIFY_IS_FALSE(isSupported);

        // A very high version should be supported.
        const WSLCCompatVersion futureVersion{99, 0, 0};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&futureVersion, &isSupported));
        VERIFY_IS_TRUE(isSupported);
    }

    WSLC_TEST_METHOD(ListSessionsReturnsSessionWithDisplayName)
    {
        auto sessionManager = OpenSessionManager();

        // Act: list sessions
        {
            const auto names = ListTestSessionNames(sessionManager.get());

            // Assert
            VERIFY_ARE_EQUAL(names.size(), 1u);

            // SessionId is implementation detail (starts at 1), so we only assert DisplayName here.
            VERIFY_IS_TRUE(names.contains(c_testSessionName));
        }

        // List multiple sessions.
        {
            auto session2 = CreateSession(GetDefaultSessionSettings(L"wslc-test-list-2"));

            const auto names = ListTestSessionNames(sessionManager.get());

            VERIFY_ARE_EQUAL(names.size(), 2u);
            VERIFY_IS_TRUE(names.contains(c_testSessionName));
            VERIFY_IS_TRUE(names.contains(L"wslc-test-list-2"));
        }
    }

    WSLC_TEST_METHOD(OpenSessionByNameFindsExistingSession)
    {
        auto sessionManager = OpenSessionManager();

        // Act: open by the same display name
        wil::com_ptr<IWSLCSession> opened;
        VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(c_testSessionName, &opened));
        VERIFY_IS_NOT_NULL(opened.get());

        // And verify we get WSLC_E_SESSION_NOT_FOUND for a nonexistent name
        wil::com_ptr<IWSLCSession> notFound;
        auto hr = sessionManager->OpenSessionByName(L"this-name-does-not-exist", &notFound);
        VERIFY_ARE_EQUAL(hr, WSLC_E_SESSION_NOT_FOUND);
    }

    WSLC_TEST_METHOD(GetDisplayNameReturnsSessionName)
    {
        wil::unique_cotaskmem_string displayName;
        VERIFY_SUCCEEDED(m_defaultSession->GetDisplayName(&displayName));
        VERIFY_IS_NOT_NULL(displayName.get());
        VERIFY_ARE_EQUAL(std::wstring(displayName.get()), c_testSessionName);
    }

    WSLC_TEST_METHOD(CreateSessionValidation)
    {
        auto sessionManager = OpenSessionManager();

        // Reject NULL DisplayName.
        {
            auto settings = GetDefaultSessionSettings(nullptr);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Reject DisplayName at exact boundary (no room for null terminator).
        {
            std::wstring boundaryName(std::size(WSLCSessionListEntry{}.DisplayName), L'x');
            auto settings = GetDefaultSessionSettings(boundaryName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Reject too long DisplayName.
        {
            std::wstring longName(std::size(WSLCSessionListEntry{}.DisplayName) + 1, L'x');
            auto settings = GetDefaultSessionSettings(longName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Validate that creating a session on a non-existing storage fails if WSLCSessionStorageFlagsNoCreate is set.
        {
            auto settings = GetDefaultSessionSettings(L"storage-not-found");
            settings.StoragePath = L"C:\\does-not-exist";
            settings.StorageFlags = WSLCSessionStorageFlagsNoCreate;
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        }

        // Reject invalid storage flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-storage-flags");
            settings.StorageFlags = static_cast<WSLCSessionStorageFlags>(0x4);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
        }

        // Reject non-empty storage directory that doesn't contain a session VHD.
        {
            const auto storagePath = std::filesystem::temp_directory_path() /
                                     std::format(L"wslc-test-storage-{}-{}", GetCurrentProcessId(), GetTickCount64());
            std::filesystem::create_directories(storagePath);
            auto cleanup = wil::scope_exit([&]() {
                std::error_code ignored;
                std::filesystem::remove_all(storagePath, ignored);
            });

            std::ofstream{storagePath / L"userfile.txt"} << "data";

            auto settings = GetDefaultSessionSettings(L"storage-not-empty");
            const auto storagePathString = storagePath.wstring();
            settings.StoragePath = storagePathString.c_str();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
            ValidateCOMErrorMessage(std::format(L"Cannot use '{}' as session storage because the directory is not empty", storagePathString));
        }

        // Reject storage path that exists but is not a directory.
        {
            const auto storagePath = std::filesystem::temp_directory_path() /
                                     std::format(L"wslc-test-storage-file-{}-{}", GetCurrentProcessId(), GetTickCount64());
            std::ofstream{storagePath} << "data";
            auto cleanup = wil::scope_exit([&]() {
                std::error_code ignored;
                std::filesystem::remove(storagePath, ignored);
            });

            auto settings = GetDefaultSessionSettings(L"storage-not-directory");
            const auto storagePathString = storagePath.wstring();
            settings.StoragePath = storagePathString.c_str();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
            ValidateCOMErrorMessage(std::format(L"Cannot use '{}' as session storage because it is not a directory", storagePathString));
        }

        // Reject invalid session flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-session-flags");
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(E_INVALIDARG, sessionManager->CreateSession(&settings, static_cast<WSLCSessionFlags>(0x4), nullptr, &session));
        }

        // Reject invalid feature flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-feature-flags");
            settings.FeatureFlags = static_cast<WSLCFeatureFlags>(0x40);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(E_INVALIDARG, sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session));
        }

        // Reject NULL output pointers across the session manager API.
        {
            auto settings = GetDefaultSessionSettings(L"null-out-session");
            VERIFY_ARE_EQUAL(
                HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->OpenSession(0, nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->OpenSessionByName(c_testSessionName, nullptr));

            WSLCSessionListEntry* entries = nullptr;
            ULONG count = 0;
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->ListSessions(nullptr, &count));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->ListSessions(&entries, nullptr));
        }

        // The session object must reject NULL output pointers.
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetId(nullptr));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetDisplayName(nullptr));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetState(nullptr));
    }

    WSLC_TEST_METHOD(VmOwnerMatchesSessionDisplayName)
    {
        // The default session (c_testSessionName) is already running from class setup.
        // Verify its display name appears as a VM owner in hcsdiag output.
        auto vms = ListVms();

        auto found = std::ranges::find_if(vms, [](const auto& vm) { return vm.Owner == c_testSessionName; });
        if (found == vms.end())
        {
            LogError("Expected VM owner '%ws' not found. Owners:", c_testSessionName);
            for (const auto& vm : vms)
            {
                LogError("  '%ws'", vm.Owner.c_str());
            }

            VERIFY_FAIL();
        }
    }
};
