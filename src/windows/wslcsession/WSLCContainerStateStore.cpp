/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.cpp

Abstract:

    Contains the implementation for WSLCContainerStateStore.

--*/

#include "precomp.h"
#include "WSLCContainerStateStore.h"

using wsl::windows::service::wslc::WSLCContainerStateLoadResult;
using wsl::windows::service::wslc::WSLCContainerStateLoadStatus;
using wsl::windows::service::wslc::WSLCContainerStateStorage;
using wsl::windows::service::wslc::WSLCContainerStateStore;
using wsl::windows::service::wslc::WSLCPersistedContainerState;

namespace {

constexpr auto c_documentExtension = L".json";
constexpr auto c_tempSuffix = L".tmp";

// Documents hold a handful of counters, so anything larger is not one of ours.
constexpr LONGLONG c_maxDocumentSize = 64 * 1024;

bool IsNotFound(DWORD Error) noexcept
{
    return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
}

WSLCContainerStateLoadResult LoadFailure(WSLCContainerStateLoadStatus Status, HRESULT Error) noexcept
{
    return {.Status = Status, .Error = Error};
}

} // namespace

std::filesystem::path wsl::windows::service::wslc::ContainerStatePath(const std::filesystem::path& Directory, const std::string& ContainerId)
{
    return Directory / (wsl::shared::string::MultiByteToWide(ContainerId) + c_documentExtension);
}

WSLCContainerStateLoadResult wsl::windows::service::wslc::LoadContainerState(const std::filesystem::path& Path) noexcept
try
{
    wil::unique_hfile file{CreateFileW(
        Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file)
    {
        const auto error = GetLastError();
        return LoadFailure(
            IsNotFound(error) ? WSLCContainerStateLoadStatus::Missing : WSLCContainerStateLoadStatus::Unreadable, HRESULT_FROM_WIN32(error));
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size))
    {
        return LoadFailure(WSLCContainerStateLoadStatus::Unreadable, HRESULT_FROM_WIN32(GetLastError()));
    }

    if (size.QuadPart > c_maxDocumentSize)
    {
        WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(size.QuadPart, "Size"));
        return LoadFailure(WSLCContainerStateLoadStatus::Corrupt, HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT));
    }

    std::string content(gsl::narrow_cast<size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(file.get(), content.data(), gsl::narrow_cast<DWORD>(content.size()), &read, nullptr))
    {
        return LoadFailure(WSLCContainerStateLoadStatus::Unreadable, HRESULT_FROM_WIN32(GetLastError()));
    }

    content.resize(read);

    auto json = nlohmann::json::parse(content, nullptr, false);
    if (json.is_discarded())
    {
        WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"));
        return LoadFailure(WSLCContainerStateLoadStatus::Corrupt, HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT));
    }

    try
    {
        return {.Status = WSLCContainerStateLoadStatus::Loaded, .State = json.get<WSLCPersistedContainerState>()};
    }
    catch (const nlohmann::json::exception& e)
    {
        WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(e.what(), "Error"));
        return LoadFailure(WSLCContainerStateLoadStatus::Corrupt, HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT));
    }
}
catch (...)
{
    return LoadFailure(WSLCContainerStateLoadStatus::Unreadable, wil::ResultFromCaughtException());
}

HRESULT wsl::windows::service::wslc::WriteContainerState(const std::filesystem::path& Path, const WSLCPersistedContainerState& State, bool ReplaceExisting) noexcept
try
{
    std::error_code error;
    std::filesystem::create_directories(Path.parent_path(), error);
    RETURN_IF_WIN32_ERROR(error.value());

    const auto content = wsl::shared::ToJson(State);
    auto tempPath = Path;
    tempPath += c_tempSuffix;

    auto removeTemp = wil::scope_exit([&tempPath]() { DeleteFileW(tempPath.c_str()); });

    {
        wil::unique_hfile file{CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        RETURN_LAST_ERROR_IF(!file);

        DWORD written{};
        RETURN_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), gsl::narrow_cast<DWORD>(content.size()), &written, nullptr));
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), written != content.size());
        RETURN_IF_WIN32_BOOL_FALSE(FlushFileBuffers(file.get()));
    }

    // The rename is atomic, so an interrupted write never leaves a truncated document behind.
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    WI_SetFlagIf(flags, MOVEFILE_REPLACE_EXISTING, ReplaceExisting);
    RETURN_IF_WIN32_BOOL_FALSE(MoveFileExW(tempPath.c_str(), Path.c_str(), flags));

    removeTemp.release();
    return S_OK;
}
CATCH_RETURN()

HRESULT wsl::windows::service::wslc::DeleteContainerState(const std::filesystem::path& Path) noexcept
{
    if (DeleteFileW(Path.c_str()))
    {
        return S_OK;
    }

    const auto error = GetLastError();
    if (IsNotFound(error))
    {
        return S_OK;
    }

    RETURN_WIN32(error);
}

void wsl::windows::service::wslc::DeleteOrphanedContainerStates(const std::filesystem::path& Directory, const std::unordered_set<std::string>& KnownContainerIds) noexcept
try
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(Directory, error))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        // Temporary files are only left behind by writes that were interrupted before their rename.
        const auto extension = entry.path().extension();
        const bool isTemp = extension == c_tempSuffix;
        if (!isTemp && extension != c_documentExtension)
        {
            continue;
        }

        if (!isTemp && KnownContainerIds.contains(wsl::shared::string::WideToMultiByte(entry.path().stem().wstring())))
        {
            continue;
        }

        WSL_LOG("DeleteOrphanedContainerState", TraceLoggingValue(entry.path().c_str(), "Path"));
        LOG_IF_FAILED(DeleteContainerState(entry.path()));
    }

    LOG_IF_WIN32_ERROR_MSG(IsNotFound(error.value()) ? ERROR_SUCCESS : error.value(), "Failed to enumerate %ls", Directory.c_str());
}
CATCH_LOG()

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForNewContainer(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId)
{
    if (!Directory.has_value())
    {
        return {};
    }

    return {.Path = ContainerStatePath(Directory.value(), ContainerId), .Pending = true};
}

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForExistingContainer(
    const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId, WSLCContainerStateStorage Storage)
{
    if (!Directory.has_value())
    {
        return {};
    }

    auto path = ContainerStatePath(Directory.value(), ContainerId);

    bool missingMeansDefault{};
    switch (Storage)
    {
    case WSLCContainerStateStorage::None:
        missingMeansDefault = true;
        break;

    case WSLCContainerStateStorage::HostFileV1:
        missingMeansDefault = false;
        break;

    default:
        return {.Path = std::move(path), .LoadError = HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE)};
    }

    const auto result = LoadContainerState(path);
    if (result.Status == WSLCContainerStateLoadStatus::Loaded)
    {
        return {.Path = std::move(path), .State = result.State, .DocumentExists = true};
    }

    if (result.Status == WSLCContainerStateLoadStatus::Missing && missingMeansDefault)
    {
        return {.Path = std::move(path)};
    }

    return {.Path = std::move(path), .LoadError = result.Error};
}

WSLCContainerStateStore::WSLCContainerStateStore(Initial&& InitialState) :
    m_path(std::move(InitialState.Path)),
    m_loadError(InitialState.LoadError),
    m_state(InitialState.State),
    m_generation(InitialState.Pending ? 1 : 0),
    m_documentExists(InitialState.DocumentExists)
{
}

WSLCPersistedContainerState WSLCContainerStateStore::Get() const
{
    std::lock_guard lock{m_stateLock};
    return m_state;
}

HRESULT WSLCContainerStateStore::Flush() noexcept
try
{
    std::lock_guard ioLock{m_ioLock};

    WSLCPersistedContainerState state;
    std::uint64_t generation{};
    bool replaceExisting{};

    {
        std::lock_guard lock{m_stateLock};
        if (!m_path.has_value() || m_deleted || m_generation == m_persistedGeneration)
        {
            return S_OK;
        }

        if (FAILED(m_loadError))
        {
            return m_loadError;
        }

        state = m_state;
        generation = m_generation;
        replaceExisting = m_documentExists;
    }

    RETURN_IF_FAILED(WriteContainerState(m_path.value(), state, replaceExisting));

    std::lock_guard lock{m_stateLock};
    m_documentExists = true;
    m_persistedGeneration = generation;
    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCContainerStateStore::Delete() noexcept
try
{
    std::lock_guard ioLock{m_ioLock};

    {
        std::lock_guard lock{m_stateLock};
        m_deleted = true;
    }

    if (!m_path.has_value())
    {
        return S_OK;
    }

    return DeleteContainerState(m_path.value());
}
CATCH_RETURN()
