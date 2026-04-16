/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCredStorageUnitTests.cpp

Abstract:

    Unit tests for the FileCredStorage and WinCredStorage credential
    storage backends. Tests Store, Get, List, and Erase operations.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "FileCredStorage.h"
#include "WinCredStorage.h"

using namespace wsl::windows::wslc::services;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCredStorageUnitTests {

class WSLCCLICredStorageUnitTests
{
    WSL_TEST_CLASS(WSLCCLICredStorageUnitTests)

    FileCredStorage m_fileStorage;
    WinCredStorage m_winCredStorage;

    static void TestStoreAndGetRoundTrips(ICredentialStorage& storage)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { storage.Erase("wslc-test-server1"); });
        storage.Store("wslc-test-server1", "test-user", "credential-data-1");

        auto [username, secret] = storage.Get("wslc-test-server1");
        VERIFY_ARE_EQUAL(std::string("test-user"), username);
        VERIFY_ARE_EQUAL(std::string("credential-data-1"), secret);
    }

    WSLC_TEST_METHOD(FileCred_Store_And_Get_RoundTrips)
    {
        TestStoreAndGetRoundTrips(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_Store_And_Get_RoundTrips)
    {
        TestStoreAndGetRoundTrips(m_winCredStorage);
    }

    static void TestGetNonExistentReturnsEmpty(ICredentialStorage& storage)
    {
        auto [username, secret] = storage.Get("wslc-test-nonexistent-server");
        VERIFY_IS_TRUE(username.empty());
        VERIFY_IS_TRUE(secret.empty());
    }

    WSLC_TEST_METHOD(FileCred_Get_NonExistent_ReturnsEmpty)
    {
        TestGetNonExistentReturnsEmpty(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_Get_NonExistent_ReturnsEmpty)
    {
        TestGetNonExistentReturnsEmpty(m_winCredStorage);
    }

    static void TestStoreOverwritesExistingCredential(ICredentialStorage& storage)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { storage.Erase("wslc-test-server2"); });
        storage.Store("wslc-test-server2", "old-user", "old-credential");
        storage.Store("wslc-test-server2", "new-user", "new-credential");

        auto [username, secret] = storage.Get("wslc-test-server2");
        VERIFY_ARE_EQUAL(std::string("new-user"), username);
        VERIFY_ARE_EQUAL(std::string("new-credential"), secret);
    }

    WSLC_TEST_METHOD(FileCred_Store_Overwrites_ExistingCredential)
    {
        TestStoreOverwritesExistingCredential(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_Store_Overwrites_ExistingCredential)
    {
        TestStoreOverwritesExistingCredential(m_winCredStorage);
    }

    static void TestListContainsStoredServers(ICredentialStorage& storage)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            storage.Erase("wslc-test-list1");
            storage.Erase("wslc-test-list2");
        });
        storage.Store("wslc-test-list1", "user1", "cred1");
        storage.Store("wslc-test-list2", "user2", "cred2");

        auto servers = storage.List();
        bool found1 = false, found2 = false;
        for (const auto& s : servers)
        {
            if (s == L"wslc-test-list1")
            {
                found1 = true;
            }
            if (s == L"wslc-test-list2")
            {
                found2 = true;
            }
        }

        VERIFY_IS_TRUE(found1);
        VERIFY_IS_TRUE(found2);
    }

    WSLC_TEST_METHOD(FileCred_List_ContainsStoredServers)
    {
        TestListContainsStoredServers(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_List_ContainsStoredServers)
    {
        TestListContainsStoredServers(m_winCredStorage);
    }

    static void TestEraseRemovesCredential(ICredentialStorage& storage)
    {
        storage.Store("wslc-test-erase", "user", "cred");
        auto [username, secret] = storage.Get("wslc-test-erase");
        VERIFY_IS_FALSE(username.empty());

        storage.Erase("wslc-test-erase");
        auto [username2, secret2] = storage.Get("wslc-test-erase");
        VERIFY_IS_TRUE(username2.empty());
    }

    WSLC_TEST_METHOD(FileCred_Erase_RemovesCredential)
    {
        TestEraseRemovesCredential(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_Erase_RemovesCredential)
    {
        TestEraseRemovesCredential(m_winCredStorage);
    }

    static void TestEraseNonExistentThrows(ICredentialStorage& storage)
    {
        VERIFY_THROWS_SPECIFIC(storage.Erase("wslc-test-nonexistent-erase"), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_NOT_SET;
        });
    }

    WSLC_TEST_METHOD(FileCred_Erase_NonExistent_Throws)
    {
        TestEraseNonExistentThrows(m_fileStorage);
    }
    WSLC_TEST_METHOD(WinCred_Erase_NonExistent_Throws)
    {
        TestEraseNonExistentThrows(m_winCredStorage);
    }
};

} // namespace WSLCCredStorageUnitTests
