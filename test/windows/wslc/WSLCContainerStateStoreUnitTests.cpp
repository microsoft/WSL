// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCContainerStateStore.h"

namespace WSLCContainerStateStoreTests {

using wsl::windows::service::wslc::WSLCContainerStateStore;

class WSLCContainerStateStoreUnitTests
{
    WSLC_TEST_CLASS(WSLCContainerStateStoreUnitTests)

    std::filesystem::path m_directory;

    TEST_METHOD_SETUP(MethodSetup)
    {
        m_directory = std::filesystem::temp_directory_path() / std::format(L"wslc-state-store-{}", GetCurrentProcessId());
        std::filesystem::remove_all(m_directory);
        std::filesystem::create_directories(m_directory);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
        return true;
    }

    static std::string ReadFile(const std::filesystem::path& Path)
    {
        std::ifstream file(Path, std::ios::binary);
        VERIFY_IS_TRUE(file.good(), Path.c_str());
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static std::uint32_t ReadRestartCount(const std::filesystem::path& Path)
    {
        return nlohmann::json::parse(ReadFile(Path)).at("V1").at("RestartCount").get<std::uint32_t>();
    }

    TEST_METHOD(ContainerStateStore_CreatesAndPersistsDocument)
    {
        const auto path = WSLCContainerStateStore::PathFor(m_directory / L"containers", "container");

        {
            WSLCContainerStateStore store(m_directory / L"containers", "container");
            VERIFY_IS_TRUE(path == store.Path().value());
            VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);
            VERIFY_ARE_EQUAL(0u, ReadRestartCount(path));

            store.Update([](auto& state) { state.RestartCount = 3; });
            VERIFY_ARE_EQUAL(3u, store.Get().RestartCount);
            VERIFY_ARE_EQUAL(3u, ReadRestartCount(path));
        }

        WSLCContainerStateStore store(m_directory / L"containers", "container");
        VERIFY_ARE_EQUAL(3u, store.Get().RestartCount);

        store.Delete();
        VERIFY_IS_FALSE(std::filesystem::exists(path));
    }

    TEST_METHOD(ContainerStateStore_UnchangedUpdateDoesNotWrite)
    {
        const auto path = WSLCContainerStateStore::PathFor(m_directory, "container");
        WSLCContainerStateStore store(m_directory, "container");
        std::filesystem::remove(path);

        store.Update([](auto&) {});
        VERIFY_IS_FALSE(std::filesystem::exists(path));
    }

    TEST_METHOD(ContainerStateStore_InvalidDocumentIsReset)
    {
        const auto path = WSLCContainerStateStore::PathFor(m_directory, "container");
        for (const auto* content :
             {"{",
              "[]",
              "{\"V1\":[]}",
              "{\"V1\":{\"RestartCount\":\"invalid\"}}",
              "{\"V1\":{\"RestartCount\":-1}}",
              "{\"V1\":{\"RestartCount\":4294967296}}",
              "{\"V1\":{\"RestartCount\":1.5}}",
              "{\"V1\":{\"RestartCount\":true}}"})
        {
            std::ofstream{path, std::ios::binary | std::ios::trunc} << content;

            WSLCContainerStateStore store(m_directory, "container");
            VERIFY_ARE_EQUAL(0u, store.Get().RestartCount);
            VERIFY_ARE_EQUAL(0u, ReadRestartCount(path));
        }
    }

    TEST_METHOD(ContainerStateStore_MissingFieldsUseDefaults)
    {
        const auto path = WSLCContainerStateStore::PathFor(m_directory, "container");
        const std::pair<const char*, std::uint32_t> cases[] = {
            {"{}", 0}, {"{\"V1\":{}}", 0}, {"{\"V1\":{\"RestartCount\":4,\"Unknown\":1},\"V2\":{}}", 4}};

        for (const auto& [content, expected] : cases)
        {
            std::ofstream{path, std::ios::binary | std::ios::trunc} << content;

            WSLCContainerStateStore store(m_directory, "container");
            VERIFY_ARE_EQUAL(expected, store.Get().RestartCount);
        }
    }

    TEST_METHOD(ContainerStateStore_NoDirectoryKeepsStateInMemory)
    {
        WSLCContainerStateStore store(std::nullopt, "container");
        VERIFY_IS_FALSE(store.Path().has_value());

        store.Update([](auto& state) { state.RestartCount++; });
        VERIFY_ARE_EQUAL(1u, store.Get().RestartCount);
        store.Delete();
    }

    TEST_METHOD(ContainerStateStore_DeleteOrphans)
    {
        const auto known = WSLCContainerStateStore::PathFor(m_directory, "known");
        const auto orphan = WSLCContainerStateStore::PathFor(m_directory, "orphan");
        const auto knownTemp = m_directory / L"known.json.tmp";
        const auto unrelated = m_directory / L"notes.txt";
        for (const auto& path : {known, orphan, knownTemp, unrelated})
        {
            std::ofstream{path} << "{}";
        }

        WSLCContainerStateStore::DeleteOrphans(m_directory, {"known"});

        VERIFY_IS_TRUE(std::filesystem::exists(known));
        VERIFY_IS_FALSE(std::filesystem::exists(orphan));
        VERIFY_IS_FALSE(std::filesystem::exists(knownTemp));
        VERIFY_IS_TRUE(std::filesystem::exists(unrelated));

        // A missing directory is not an error.
        WSLCContainerStateStore::DeleteOrphans(m_directory / L"missing", {});
    }
};

} // namespace WSLCContainerStateStoreTests
