// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCContainerStateStore.h"

namespace WSLCContainerStateStoreTests {

using wsl::windows::service::wslc::ContainerStatePath;
using wsl::windows::service::wslc::DeleteOrphanedContainerStates;
using wsl::windows::service::wslc::LoadContainerState;
using wsl::windows::service::wslc::WriteContainerState;
using wsl::windows::service::wslc::WSLCContainerStateLoadStatus;
using wsl::windows::service::wslc::WSLCContainerStateStorage;
using wsl::windows::service::wslc::WSLCContainerStateStore;

class WSLCContainerStateStoreUnitTests
{
    WSLC_TEST_CLASS(WSLCContainerStateStoreUnitTests)

    std::filesystem::path m_directory;
    std::filesystem::path m_path;

    TEST_METHOD_SETUP(MethodSetup)
    {
        m_directory = std::filesystem::temp_directory_path() / std::format(L"wslc-state-store-{}", GetCurrentProcessId());
        std::filesystem::remove_all(m_directory);
        m_path = ContainerStatePath(m_directory, "container");
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
        return true;
    }

    static std::string ReadDocument(const std::filesystem::path& Path)
    {
        std::ifstream file(Path, std::ios::binary);
        VERIFY_IS_TRUE(file.good(), Path.c_str());
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static void WriteDocument(const std::filesystem::path& Path, const std::string& Content)
    {
        std::filesystem::create_directories(Path.parent_path());
        std::ofstream file(Path, std::ios::binary | std::ios::trunc);
        file << Content;
        VERIFY_IS_TRUE(file.good(), Path.c_str());
    }

    static std::uint32_t ReadRestartCount(const std::filesystem::path& Path)
    {
        return nlohmann::json::parse(ReadDocument(Path)).at("RestartCount").get<std::uint32_t>();
    }

    void VerifyCorrupt(const std::string& Content)
    {
        WriteDocument(m_path, Content);
        const auto result = LoadContainerState(m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Corrupt, result.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), result.Error);
    }

    TEST_METHOD(ContainerStateStore_LoadDistinguishesStatus)
    {
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Missing, LoadContainerState(m_path).Status);

        WriteDocument(m_path, R"({"RestartCount":7})");
        const auto loaded = LoadContainerState(m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Loaded, loaded.Status);
        VERIFY_ARE_EQUAL(7u, loaded.State.RestartCount);

        VerifyCorrupt("{");
        VerifyCorrupt("[]");
        VerifyCorrupt(R"({"RestartCount":-1})");
        VerifyCorrupt(R"({"RestartCount":1.5})");
        VerifyCorrupt(R"({"RestartCount":4294967296})");
        VerifyCorrupt(std::string(65 * 1024, ' '));

        WriteDocument(m_path, "{}");
        wil::unique_hfile exclusive{CreateFileW(m_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_TRUE(!!exclusive);
        const auto unreadable = LoadContainerState(m_path);
        VERIFY_ARE_EQUAL(WSLCContainerStateLoadStatus::Unreadable, unreadable.Status);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), unreadable.Error);
    }

    TEST_METHOD(ContainerStateStore_NewContainerPersistsAndReloads)
    {
        {
            WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_directory, "container"));
            VERIFY_SUCCEEDED(store.Flush());
            VERIFY_ARE_EQUAL(0u, ReadRestartCount(m_path));

            store.Mutate([](auto& state) { state.RestartCount = 3; });
            VERIFY_SUCCEEDED(store.Flush());
            VERIFY_ARE_EQUAL(3u, ReadRestartCount(m_path));
        }

        auto initial = WSLCContainerStateStore::ForExistingContainer(m_directory, "container", WSLCContainerStateStorage::HostFileV1);
        VERIFY_SUCCEEDED(initial.LoadError);
        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(3u, store.Get().RestartCount);
    }

    TEST_METHOD(ContainerStateStore_NewContainerDoesNotOverwriteExistingDocument)
    {
        WriteDocument(m_path, R"({"RestartCount":9})");

        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_directory, "container"));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), store.Flush());
        VERIFY_ARE_EQUAL(9u, ReadRestartCount(m_path));
    }

    TEST_METHOD(ContainerStateStore_MissingDocumentWithoutStorageUsesDefaults)
    {
        auto initial = WSLCContainerStateStore::ForExistingContainer(m_directory, "container", WSLCContainerStateStorage::None);
        VERIFY_SUCCEEDED(initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_IS_FALSE(std::filesystem::exists(m_path));

        store.Mutate([](auto& state) { state.RestartCount = 2; });
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(2u, ReadRestartCount(m_path));
    }

    TEST_METHOD(ContainerStateStore_MissingExpectedDocumentBlocksWrites)
    {
        std::filesystem::create_directories(m_directory);
        auto initial = WSLCContainerStateStore::ForExistingContainer(m_directory, "container", WSLCContainerStateStorage::HostFileV1);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), store.Flush());
        VERIFY_IS_FALSE(std::filesystem::exists(m_path));
    }

    TEST_METHOD(ContainerStateStore_UnknownStorageBlocksWrites)
    {
        WriteDocument(m_path, R"({"RestartCount":4})");

        auto initial =
            WSLCContainerStateStore::ForExistingContainer(m_directory, "container", static_cast<WSLCContainerStateStorage>(99));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE), store.Flush());
        VERIFY_ARE_EQUAL(4u, ReadRestartCount(m_path));
    }

    TEST_METHOD(ContainerStateStore_CorruptDocumentIsPreservedUntilDelete)
    {
        const std::string corrupt = "not json";
        WriteDocument(m_path, corrupt);

        auto initial = WSLCContainerStateStore::ForExistingContainer(m_directory, "container", WSLCContainerStateStorage::HostFileV1);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), initial.LoadError);

        WSLCContainerStateStore store(std::move(initial));
        VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);

        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT), store.Flush());
        VERIFY_ARE_EQUAL(corrupt, ReadDocument(m_path));

        VERIFY_SUCCEEDED(store.Delete());
        VERIFY_IS_FALSE(std::filesystem::exists(m_path));
    }

    TEST_METHOD(ContainerStateStore_FailedWriteRemainsPending)
    {
        // A file in place of the directory makes every write fail.
        WriteDocument(m_directory, "");

        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_directory, "container"));
        store.Mutate([](auto& state) { state.RestartCount = 5; });
        VERIFY_FAILED(store.Flush());

        store.Mutate([](auto& state) { state.RestartCount = 5; });
        std::filesystem::remove(m_directory);

        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(5u, ReadRestartCount(m_path));
    }

    TEST_METHOD(ContainerStateStore_DeleteIsFinal)
    {
        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(m_directory, "container"));
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_IS_TRUE(std::filesystem::exists(m_path));

        VERIFY_SUCCEEDED(store.Delete());
        VERIFY_IS_FALSE(std::filesystem::exists(m_path));

        store.Mutate([](auto& state) { state.RestartCount = 1; });
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_IS_FALSE(std::filesystem::exists(m_path));
        VERIFY_SUCCEEDED(store.Delete());
    }

    TEST_METHOD(ContainerStateStore_WithoutDirectoryKeepsStateInMemory)
    {
        WSLCContainerStateStore store(WSLCContainerStateStore::ForNewContainer(std::nullopt, "container"));
        store.Mutate([](auto& state) { state.RestartCount = 6; });
        VERIFY_SUCCEEDED(store.Flush());
        VERIFY_ARE_EQUAL(6u, store.Get().RestartCount);
        VERIFY_SUCCEEDED(store.Delete());
        VERIFY_IS_FALSE(std::filesystem::exists(m_directory));
    }

    TEST_METHOD(ContainerStateStore_DeletesOrphanedDocuments)
    {
        VERIFY_SUCCEEDED(WriteContainerState(ContainerStatePath(m_directory, "known"), {.RestartCount = 1}, false));
        VERIFY_SUCCEEDED(WriteContainerState(ContainerStatePath(m_directory, "orphan"), {}, false));
        WriteDocument(m_directory / L"known.json.tmp", "");
        WriteDocument(m_directory / L"unrelated.txt", "");

        DeleteOrphanedContainerStates(m_directory, {"known"});

        VERIFY_ARE_EQUAL(1u, ReadRestartCount(ContainerStatePath(m_directory, "known")));
        VERIFY_IS_FALSE(std::filesystem::exists(ContainerStatePath(m_directory, "orphan")));
        VERIFY_IS_FALSE(std::filesystem::exists(m_directory / L"known.json.tmp"));
        VERIFY_IS_TRUE(std::filesystem::exists(m_directory / L"unrelated.txt"));

        DeleteOrphanedContainerStates(m_directory / L"missing", {});
    }
};

} // namespace WSLCContainerStateStoreTests
