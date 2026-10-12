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
    if (m_seededSessions.contains(sessionName))
    {
        return;
    }

    auto result = RunWslc(FormatCommand(sessionName, L"image list --format json"));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    const auto images = ParseNdjsonOutputAs<wsl::windows::wslc::models::ImageOutputInformation>(result);

    for (const auto& image : images)
    {
        if (image.Repository != wsl::windows::wslc::models::c_none && image.Tag != wsl::windows::wslc::models::c_none)
        {
            m_loaded.insert(ImageKey{
                sessionName, wsl::shared::string::MultiByteToWide(image.Repository), wsl::shared::string::MultiByteToWide(image.Tag)});
        }
    }

    m_seededSessions.insert(sessionName);
}

void TestImageRegistry::EnsureLoaded(const TestImage& image, const std::wstring& sessionName)
{
    EnsureSeeded(sessionName);

    if (m_loaded.contains(MakeKey(image, sessionName)))
    {
        return;
    }

    Load(image, sessionName);
}

void TestImageRegistry::Restore(const TestImage& image, const std::wstring& sessionName)
{
    EnsureSeeded(sessionName);
    m_loaded.erase(MakeKey(image, sessionName));
    Load(image, sessionName);
}

void TestImageRegistry::InvalidateSession(const std::wstring& sessionName)
{
    std::erase_if(m_loaded, [&](const auto& image) { return image.SessionName == sessionName; });
    m_seededSessions.erase(sessionName);
}

void TestImageRegistry::Load(const TestImage& image, const std::wstring& sessionName)
{
    auto result = RunWslc(FormatCommand(sessionName, std::format(L"image load --input \"{}\"", image.Path.wstring())));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    m_loaded.insert(MakeKey(image, sessionName));
}

void TestImageRegistry::Delete(const TestImage& image, const std::wstring& sessionName)
{
    auto result = RunWslc(FormatCommand(sessionName, L"image list --format json"));
    result.Verify({.Stderr = L"", .ExitCode = 0});

    const auto images = ParseNdjsonOutputAs<wsl::windows::wslc::models::ImageOutputInformation>(result);
    const auto name = wsl::shared::string::WideToMultiByte(image.Name);
    const auto tag = wsl::shared::string::WideToMultiByte(image.Tag);
    const bool present =
        std::ranges::any_of(images, [&](const auto& candidate) { return candidate.Repository == name && candidate.Tag == tag; });

    if (!present)
    {
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

    m_loaded.erase(MakeKey(image, sessionName));
}

} // namespace WSLCE2ETests
