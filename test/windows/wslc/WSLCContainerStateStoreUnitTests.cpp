// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCContainerStateStore.h"

namespace WSLCContainerStateStoreTests {

using wsl::windows::service::wslc::ContainerStatePath;
using wsl::windows::service::wslc::IWSLCContainerStateFiles;
using wsl::windows::service::wslc::LoadContainerState;
using wsl::windows::service::wslc::WSLCContainerStateLoadStatus;
using wsl::windows::service::wslc::WSLCContainerStateStorage;
using wsl::windows::service::wslc::WSLCContainerStateStore;

// In-memory stand-in for the guest files. Writes fail with ENOENT when the parent directory is missing.
class FakeStateFiles : public IWSLCContainerStateFiles
{
public:
    int Read(const std::string& Path, size_t MaxSize, std::string& Content) override
    {
        THROW_HR_IF(ThrowError, FAILED(ThrowError));
        if (ReadError != 0)
        {
            return ReadError;
        }

        const auto file = Files.find(Path);
        if (file == Files.end())
        {
            return ENOENT;
        }

        if (file->second.size() > MaxSize)
        {
            return EFBIG;
        }

        Content = file->second;
        return 0;
    }

    int Write(const std::string& Path, std::string_view Content) override
    {
        if (BeforeWrite)
        {
            BeforeWrite();
        }

        THROW_HR_IF(ThrowError, FAILED(ThrowError));
        if (WriteError != 0)
        {
            return WriteError;
        }

        if (!Directories.contains(Path.substr(0, Path.rfind('/'))))
        {
            return ENOENT;
        }

        Files[Path] = Content;
        Writes++;
        return 0;
    }

    std::map<std::string, std::string> Files;
    std::set<std::string> Directories;
    int ReadError = 0;
    int WriteError = 0;
    HRESULT ThrowError = S_OK;
    size_t Writes = 0;
    std::function<void()> BeforeWrite;
};

class WSLCContainerStateStoreUnitTests
{
    WSLC_TEST_CLASS(WSLCContainerStateStoreUnitTests)

    const std::string m_id = std::string(64, 'a');
    std::string m_path;
    std::shared_ptr<FakeStateFiles> m_files;

    TEST_METHOD_SETUP(MethodSetup)
    {
        m_path = ContainerStatePath(m_id);
        m_files = std::make_shared<FakeStateFiles>();
        m_files->Directories.insert(m_path.substr(0, m_path.rfind('/')));
        return true;
    }

    std::uint32_t ReadRestartCount() const
    {
        const auto file = m_files->Files.find(m_path);
        VERIFY_IS_TRUE(file != m_files->Files.end());
        return nlohmann::json::parse(file->second).at("RestartCount").get<std::uint32_t>();
    }

    void VerifyCorrupt(const std::string& Content)
    {
        m_files->Files[m_path] = Content;
        const auto result = LoadContainerState(*m_files, m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Corrupt, result.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), result.Error);
    }

    void VerifyUnreadable(HRESULT Expected)
    {
        const auto result = LoadContainerState(*m_files, m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Unreadable, result.Status);
        VERIFY_ARE_EQUAL(Expected, result.Error);
    }

    TEST_METHOD(ContainerStateStore_PathRequiresContainerId)
    {
        VERIFY_ARE_EQUAL(std::format("/var/lib/docker/containers/{}/wslc-state.json", m_id), m_path);

        for (const auto& id :
             {std::string{"container"}, std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g'), "../" + std::string(61, 'a')})
        {
            VERIFY_THROWS_SPECIFIC(ContainerStatePath(id), wil::ResultException, [](const wil::ResultException& e) {
                return e.GetErrorCode() == E_INVALIDARG;
            });
        }
    }

    TEST_METHOD(ContainerStateStore_LoadDistinguishesStatus)
    {
        const auto missing = LoadContainerState(*m_files, m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Missing, missing.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), missing.Error);

        m_files->Files[m_path] = R"({"RestartCount":7})";
        const auto loaded = LoadContainerState(*m_files, m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Loaded, loaded.Status);
        VERIFY_ARE_EQUAL(7u, loaded.State.RestartCount);

        VerifyCorrupt("{");
        VerifyCorrupt("[]");
        VerifyCorrupt(R"({"RestartCount":-1})");
        VerifyCorrupt(R"({"RestartCount":1.5})");
        VerifyCorrupt(R"({"RestartCount":4294967296})");
        VerifyCorrupt(std::string(65 * 1024, ' '));

        m_files->Files[m_path] = "{}";
        m_files->ReadError = EIO;
        VerifyUnreadable(E_FAIL);

        m_files->ReadError = EACCES;
        VerifyUnreadable(E_ACCESSDENIED);

        m_files->ReadError = 0;
        m_files->ThrowError = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        VerifyUnreadable(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
    }

    TEST_METHOD(ContainerStateStore_NewContainerPersistsAndReloads)
    {
        {
            WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_files, m_id));
            VERIFY_SUCCEEDED(store.Flush());
            VERIFY_ARE_EQUAL(0u, ReadRestartCount());

            store.Mutate([](auto& state) { state.RestartCount = 3; });
            VERIFY_SUCCEEDED(store.Flush());
            VERIFY_ARE_EQUAL(3u, ReadRestartCount());

            VERIFY_SUCCEEDED(store.Flush());
            VERIFY_ARE_EQUAL(2u, m_files->Writes);
        }

        auto initial = WSLCContainerStateStore::ForExistingContainer(m_files, m_id, WSLCContainerStateStorage::GuestFileV1);
        VERIFY_SUCCEEDED(initial.LoadError);
        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(3u, store.Get().RestartCount);
    }

    TEST_METHOD(ContainerStateStore_MissingDocumentWithoutStorageUsesDefaults)
    {
        auto initial = WSLCContainerStateStore::ForExistingContainer(m_files, m_id, WSLCContainerStateStorage::None);
        VERIFY_SUCCEEDED(initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_IS_FALSE(m_files->Files.contains(m_path));

        store.Mutate([](auto& state) { state.RestartCount = 2; });
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(2u, ReadRestartCount());
    }

    TEST_METHOD(ContainerStateStore_MissingExpectedDocumentBlocksWrites)
    {
        auto initial = WSLCContainerStateStore::ForExistingContainer(m_files, m_id, WSLCContainerStateStorage::GuestFileV1);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), store.Flush());
        VERIFY_ARE_EQUAL(0u, m_files->Writes);
    }

    TEST_METHOD(ContainerStateStore_UnknownStorageBlocksWrites)
    {
        m_files->Files[m_path] = R"({"RestartCount":4})";

        auto initial = WSLCContainerStateStore::ForExistingContainer(m_files, m_id, static_cast<WSLCContainerStateStorage>(99));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE), store.Flush());
        VERIFY_ARE_EQUAL(4u, ReadRestartCount());
    }

    TEST_METHOD(ContainerStateStore_CorruptDocumentIsPreserved)
    {
        const std::string corrupt = "not json";
        m_files->Files[m_path] = corrupt;

        auto initial = WSLCContainerStateStore::ForExistingContainer(m_files, m_id, WSLCContainerStateStorage::GuestFileV1);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);

        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), store.Flush());
        VERIFY_ARE_EQUAL(corrupt, m_files->Files[m_path]);

        store.MarkDeleted();
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(0u, m_files->Writes);
    }

    TEST_METHOD(ContainerStateStore_FailedWriteRemainsPending)
    {
        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_files, m_id));
        store.Mutate([](auto& state) { state.RestartCount = 5; });

        m_files->WriteError = ENOSPC;
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_DISK_FULL), store.Flush());

        m_files->WriteError = 0;
        m_files->ThrowError = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE), store.Flush());
        VERIFY_IS_FALSE(m_files->Files.contains(m_path));

        // Setting the same value again doesn't change anything, so only the pending write can make the next flush write.
        store.Mutate([](auto& state) { state.RestartCount = 5; });
        m_files->ThrowError = S_OK;
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(5u, ReadRestartCount());
    }

    TEST_METHOD(ContainerStateStore_WriteDoesNotCreateDirectory)
    {
        m_files->Directories.clear();

        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_files, m_id));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), store.Flush());
        VERIFY_IS_FALSE(m_files->Files.contains(m_path));
    }

    TEST_METHOD(ContainerStateStore_DeleteIsFinal)
    {
        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_files, m_id));
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(1u, m_files->Writes);

        store.MarkDeleted();
        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(1u, m_files->Writes);
        VERIFY_ARE_EQUAL(0u, ReadRestartCount());
        store.MarkDeleted();
    }

    TEST_METHOD(ContainerStateStore_WriteRacingDeleteSucceeds)
    {
        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_files, m_id));

        // The container is removed while the write is in flight, taking its directory with it.
        m_files->BeforeWrite = [&]() {
            store.MarkDeleted();
            m_files->Directories.clear();
        };

        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_IS_FALSE(m_files->Files.contains(m_path));
    }
};

} // namespace WSLCContainerStateStoreTests
