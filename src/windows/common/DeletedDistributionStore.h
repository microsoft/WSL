// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace wsl::windows::common {

// Callers serialize operations and impersonate the distribution's owner.
class DeletedDistributionStore
{
public:
    struct Entry
    {
        GUID Id;
        std::wstring Name;
        std::filesystem::path Path;
        ULONG64 DeletedAt;
    };

    static constexpr ULONG64 c_retention = 24ULL * 60 * 60 * 10000000;
    static ULONG64 Now();
    // Cleanup receives the verified physical source directory before the VHD moves or journaling starts.
    static bool Retain(
        HKEY LxssKey,
        const GUID& Id,
        const std::filesystem::path& VhdPath,
        const std::function<void(const std::filesystem::path&)>& CleanupArtifacts = {});
    static std::vector<Entry> Enumerate(HKEY LxssKey, bool IncludePermanentDelete = false);
    static void Restore(HKEY LxssKey, const Entry& Distribution, LPCWSTR Name);
    static void Purge(HKEY LxssKey, const GUID& Id);
    // Background callers may yield their operation lock between records. The
    // callback must reacquire it before returning true; false stops this pass.
    static void RecoverPending(HKEY LxssKey, const std::function<bool()>& YieldBetweenEntries = {}) noexcept;
    static void Cleanup(HKEY LxssKey, ULONG64 CurrentTime = Now(), const std::function<bool()>& YieldBetweenEntries = {}) noexcept;
};

} // namespace wsl::windows::common
