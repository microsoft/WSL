/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TestImageRegistry.cpp

Abstract:

    This file contains the implementation of the test image registry.
--*/

#include "precomp.h"
#include "ImageModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {

std::wstring TestImageRegistry::FormatCommand(const std::wstring& sessionName, const std::wstring& command)
{
    if (sessionName.empty())
    {
        return command;
    }

    return std::format(L"--session \"{}\" {}", sessionName, command);
}

std::wstring TestImageRegistry::ParseSessionName(const std::wstring& commandLine)
{
    constexpr std::wstring_view sessionFlag = L"--session ";
    const auto flagOffset = commandLine.find(sessionFlag);
    if (flagOffset == std::wstring::npos)
    {
        return {};
    }

    auto valueOffset = commandLine.find_first_not_of(L' ', flagOffset + sessionFlag.size());
    if (valueOffset == std::wstring::npos)
    {
        return {};
    }

    if (commandLine[valueOffset] == L'"')
    {
        const auto closingQuote = commandLine.find(L'"', valueOffset + 1);
        if (closingQuote == std::wstring::npos)
        {
            return {};
        }

        return commandLine.substr(valueOffset + 1, closingQuote - valueOffset - 1);
    }

    const auto end = commandLine.find(L' ', valueOffset);
    return commandLine.substr(valueOffset, end == std::wstring::npos ? std::wstring::npos : end - valueOffset);
}

bool TestImageRegistry::CommandCanRemoveImages(const std::wstring& commandLine)
{
    constexpr std::wstring_view removalCommands[] = {
        L"image remove", L"image delete", L"image rm", L"image prune", L"session terminate"};
    return std::ranges::any_of(
        removalCommands, [&](const auto& command) { return commandLine.find(command) != std::wstring::npos; });
}

std::vector<TestImageRegistry::ImageKey> TestImageRegistry::ParseRemovedImages(const std::wstring& commandLine, const std::wstring& sessionName)
{
    constexpr std::wstring_view targetedCommands[] = {L"image remove", L"image delete", L"image rm"};

    auto offset = std::wstring::npos;
    for (const auto& command : targetedCommands)
    {
        const auto found = commandLine.find(command);
        if (found != std::wstring::npos)
        {
            offset = found + command.size();
            break;
        }
    }

    if (offset == std::wstring::npos)
    {
        return {};
    }

    std::vector<ImageKey> images;
    for (auto token : wsl::shared::string::Split(commandLine.substr(offset), L' '))
    {
        if (token.starts_with(L'-'))
        {
            continue;
        }

        if (token.size() >= 2 && token.front() == L'"' && token.back() == L'"')
        {
            token = token.substr(1, token.size() - 2);
        }

        // Only a plain name:tag operand maps onto a cache key. A digest, or a bare name whose only
        // colon introduces a registry port, leaves the affected set unknown, so the caller has to
        // fall back to dropping the whole session.
        const auto separator = token.rfind(L':');
        if (token.find(L'@') != std::wstring::npos || separator == std::wstring::npos || token.find(L'/', separator) != std::wstring::npos)
        {
            return {};
        }

        images.push_back(ImageKey{sessionName, token.substr(0, separator), token.substr(separator + 1)});
    }

    return images;
}

TestImageRegistry& TestImageRegistry::Instance()
{
    static TestImageRegistry registry;
    return registry;
}

TestImageRegistry::ImageKey TestImageRegistry::MakeKey(const TestImage& image, const std::wstring& sessionName)
{
    return ImageKey{sessionName, image.Name, image.Tag};
}

void TestImageRegistry::EnsureSeeded(const std::wstring& sessionName)
{
    {
        auto lock = m_lock.lock_shared();
        if (m_seededSessions.contains(sessionName))
        {
            return;
        }
    }

    auto result = RunWslc(FormatCommand(sessionName, L"image list --format json"));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    const auto images = ParseNdjsonOutputAs<wsl::windows::wslc::models::ImageInformation>(result);

    auto lock = m_lock.lock_exclusive();
    for (const auto& image : images)
    {
        if (image.Repository && image.Tag)
        {
            m_loaded.insert(ImageKey{
                sessionName, wsl::shared::string::MultiByteToWide(*image.Repository), wsl::shared::string::MultiByteToWide(*image.Tag)});
        }
    }

    m_seededSessions.insert(sessionName);
}

void TestImageRegistry::EnsureLoaded(const TestImage& image, const std::wstring& sessionName)
{
    EnsureSeeded(sessionName);

    {
        auto lock = m_lock.lock_shared();
        if (m_loaded.contains(MakeKey(image, sessionName)))
        {
            return;
        }
    }

    Load(image, sessionName);
}

void TestImageRegistry::Load(const TestImage& image, const std::wstring& sessionName)
{
    auto result = RunWslc(FormatCommand(sessionName, std::format(L"image load --input \"{}\"", image.Path.wstring())));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto lock = m_lock.lock_exclusive();
    m_loaded.insert(MakeKey(image, sessionName));
}

void TestImageRegistry::Delete(const TestImage& image, const std::wstring& sessionName)
{
    auto result = RunWslc(FormatCommand(sessionName, L"image list --format json"));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    const auto images = ParseNdjsonOutputAs<wsl::windows::wslc::models::ImageInformation>(result);
    const auto name = wsl::shared::string::WideToMultiByte(image.Name);
    const auto tag = wsl::shared::string::WideToMultiByte(image.Tag);
    const bool present =
        std::ranges::any_of(images, [&](const auto& candidate) { return candidate.Repository == name && candidate.Tag == tag; });

    if (!present)
    {
        auto lock = m_lock.lock_exclusive();
        m_loaded.erase(MakeKey(image, sessionName));
        return;
    }

    // Container enumeration is scoped to the default session, so named sessions rely on the
    // force flag to remove the image out from under any containers still referencing it.
    if (sessionName.empty())
    {
        EnsureImageContainersAreDeleted(image);
    }

    auto deleteResult = RunWslc(FormatCommand(sessionName, std::format(L"image delete --force {}", image.NameAndTag())));
    deleteResult.Verify({.Stderr = L"", .ExitCode = 0});

    auto lock = m_lock.lock_exclusive();
    m_loaded.erase(MakeKey(image, sessionName));
}

void TestImageRegistry::NoteCommand(const std::wstring& commandLine)
{
    if (!CommandCanRemoveImages(commandLine))
    {
        return;
    }

    const auto sessionName = ParseSessionName(commandLine);

    // Commands that name their targets only cost those cache entries. Keeping the session seeded is
    // what makes the cache worthwhile, so it is dropped only when the affected set cannot be known.
    const auto images = ParseRemovedImages(commandLine, sessionName);
    if (images.empty())
    {
        InvalidateSession(sessionName);
        return;
    }

    auto lock = m_lock.lock_exclusive();
    for (const auto& image : images)
    {
        m_loaded.erase(image);
    }
}

void TestImageRegistry::InvalidateSession(const std::wstring& sessionName)
{
    auto lock = m_lock.lock_exclusive();
    std::erase_if(m_loaded, [&](const ImageKey& key) { return key.SessionName == sessionName; });
    m_seededSessions.erase(sessionName);
}

} // namespace WSLCE2ETests
