/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.cpp

Abstract:

    Contains the implementation for WSLCContainerStateStore.

--*/

#include "precomp.h"
#include "WSLCContainerStateStore.h"
#include <fstream>

using wsl::windows::service::wslc::WSLCContainerStateDocument;
using wsl::windows::service::wslc::WSLCContainerStateStore;
using wsl::windows::service::wslc::WSLCContainerStateV1;

namespace {

constexpr auto c_stateFileExtension = L".json";
constexpr auto c_tempFileExtension = L".json.tmp";
constexpr auto c_tempExtension = L".tmp";

std::optional<WSLCContainerStateV1> LoadState(const std::filesystem::path& Path)
{
    std::ifstream file(Path, std::ios::binary);
    if (!file)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), std::filesystem::exists(Path));
        return {};
    }

    const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    // N.B. Parse without exceptions so a corrupt document does not surface a user-facing error.
    // Corrupt documents are reported as missing so they get rewritten with default state.
    auto json = nlohmann::json::parse(content, nullptr, false);
    if (json.is_discarded())
    {
        WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"));
        return {};
    }

    try
    {
        return json.get<WSLCContainerStateDocument>().V1.value_or(WSLCContainerStateV1{});
    }
    catch (const nlohmann::json::exception& e)
    {
        WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(e.what(), "Error"));
        return {};
    }
}

} // namespace

WSLCContainerStateStore::WSLCContainerStateStore(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId)
{
    if (!Directory.has_value())
    {
        return;
    }

    m_path = PathFor(Directory.value(), ContainerId);

    try
    {
        if (auto state = LoadState(m_path.value()))
        {
            m_state = state.value();
            return;
        }

        Save();
    }
    CATCH_LOG_MSG("Failed to load state for container '%hs'", ContainerId.c_str());
}

std::filesystem::path WSLCContainerStateStore::PathFor(const std::filesystem::path& Directory, const std::string& ContainerId)
{
    return Directory / (wsl::shared::string::MultiByteToWide(ContainerId) + c_stateFileExtension);
}

void WSLCContainerStateStore::Update(const std::function<void(WSLCContainerStateV1&)>& Mutate) noexcept
try
{
    auto state = m_state;
    Mutate(state);
    if (state == m_state)
    {
        return;
    }

    m_state = state;
    Save();
}
CATCH_LOG()

void WSLCContainerStateStore::Save() const
{
    if (!m_path.has_value())
    {
        return;
    }

    std::filesystem::create_directories(m_path->parent_path());

    const auto content = wsl::shared::ToJson(WSLCContainerStateDocument{.V1 = m_state});
    auto tempPath = m_path.value();
    tempPath.replace_extension(c_tempFileExtension);

    {
        wil::unique_hfile file{CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!file);

        DWORD written{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), gsl::narrow_cast<DWORD>(content.size()), &written, nullptr));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), written != content.size());
        THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(file.get()));
    }

    // Replace atomically so an interrupted write never leaves a truncated document behind.
    THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(tempPath.c_str(), m_path->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

void WSLCContainerStateStore::Delete() noexcept
{
    if (!m_path.has_value())
    {
        return;
    }

    std::error_code error;
    std::filesystem::remove(m_path.value(), error);
    if (error)
    {
        LOG_HR_MSG(HRESULT_FROM_WIN32(error.value()), "Failed to delete container state: %ls", m_path->c_str());
    }
}

void WSLCContainerStateStore::DeleteOrphans(const std::filesystem::path& Directory, const std::unordered_set<std::string>& KnownContainerIds) noexcept
try
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(Directory, error))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        // Leftover temporary files come from writes interrupted before the atomic replace.
        const auto extension = entry.path().extension();
        if (extension != c_stateFileExtension && extension != c_tempExtension)
        {
            continue;
        }

        const auto containerId = wsl::shared::string::WideToMultiByte(entry.path().stem().wstring());
        if (extension == c_stateFileExtension && KnownContainerIds.contains(containerId))
        {
            continue;
        }

        WSL_LOG("DeleteOrphanedContainerState", TraceLoggingValue(entry.path().c_str(), "Path"));

        std::error_code removeError;
        std::filesystem::remove(entry.path(), removeError);
        LOG_HR_IF_MSG(HRESULT_FROM_WIN32(removeError.value()), !!removeError, "Failed to delete %ls", entry.path().c_str());
    }
}
CATCH_LOG()
