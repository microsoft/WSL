/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TestImageRegistry.h

Abstract:

    This file contains a per-process registry that tracks which test images are loaded in a
    session, so that repeated setup across test classes does not reload the same image tar.
--*/

#pragma once

#include <set>
#include <string>
#include <tuple>

namespace WSLCE2ETests {

struct TestImage;

// The cache is only correct as long as every removal is either routed through Delete or followed
// by Restore, so a test that removes images another way has to put the session back itself.
class TestImageRegistry
{
public:
    static TestImageRegistry& Instance();

    NON_COPYABLE(TestImageRegistry);
    NON_MOVABLE(TestImageRegistry);

    // Loads the image only if it is not already present in the session.
    void EnsureLoaded(const TestImage& image, const std::wstring& sessionName = L"");

    // Deletes the image if present, along with any containers using it, and drops it from the cache.
    // The image inventory is queried directly rather than through the cache, so images created outside
    // the registry, such as built or imported ones, are still removed.
    void Delete(const TestImage& image, const std::wstring& sessionName = L"");

    // Loads the image back without consulting the cache. Tests that remove images without going
    // through Delete, such as those exercising image prune, call this to restore the session.
    void Restore(const TestImage& image, const std::wstring& sessionName = L"");

    // Discards cached image state for a session after an operation that can remove multiple images.
    void InvalidateSession(const std::wstring& sessionName = L"");

private:
    TestImageRegistry() = default;

    // Loads the image without consulting the cache.
    void Load(const TestImage& image, const std::wstring& sessionName);

    struct ImageKey
    {
        std::wstring SessionName;
        std::wstring Name;
        std::wstring Tag;

        bool operator<(const ImageKey& other) const
        {
            return std::tie(SessionName, Name, Tag) < std::tie(other.SessionName, other.Name, other.Tag);
        }
    };

    static ImageKey MakeKey(const TestImage& image, const std::wstring& sessionName);

    // Populates the cache for a session from a live image list query the first time it is used.
    void EnsureSeeded(const std::wstring& sessionName);

    // Prefixes a command with the session flag when the command targets a named session.
    static std::wstring FormatCommand(const std::wstring& sessionName, const std::wstring& command);

    // Test methods run one at a time in a single process, so the cache needs no synchronization.
    std::set<ImageKey> m_loaded;
    std::set<std::wstring> m_seededSessions;
};

} // namespace WSLCE2ETests
