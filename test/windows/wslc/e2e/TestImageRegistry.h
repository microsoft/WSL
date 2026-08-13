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
#include <vector>

namespace WSLCE2ETests {

struct TestImage;

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

    // Observes a wslc command line and drops cached state that the command may have invalidated.
    // Called for every wslc invocation so that the cache cannot report an image as loaded after
    // something else removed it.
    void NoteCommand(const std::wstring& commandLine);

private:
    TestImageRegistry() = default;

    // Loads the image without consulting the cache.
    void Load(const TestImage& image, const std::wstring& sessionName);

    // Drops every cached entry for a session, so the next lookup re-queries the live inventory.
    void InvalidateSession(const std::wstring& sessionName);

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

    // Extracts the session a command targets, matching the --session "name" form the helpers emit.
    static std::wstring ParseSessionName(const std::wstring& commandLine);

    // Lists the images a removal command targets. An empty result means the affected set is
    // unbounded or not expressible as cache keys, so the whole session has to be dropped.
    static std::vector<ImageKey> ParseRemovedImages(const std::wstring& commandLine, const std::wstring& sessionName);

    // Only commands that can remove an image matter. Commands that add one can at worst leave the
    // cache pessimistic, which costs an extra query rather than producing a wrong answer.
    static bool CommandCanRemoveImages(const std::wstring& commandLine);

    mutable wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::set<ImageKey> m_loaded;
    _Guarded_by_(m_lock) std::set<std::wstring> m_seededSessions;
};

} // namespace WSLCE2ETests
