// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "WslCoreConfig.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

class TemporaryConfigFile final
{
public:
    explicit TemporaryConfigFile(std::string_view contents)
    {
        std::array<wchar_t, MAX_PATH> tempDirectory{};
        const auto tempDirectoryLength = GetTempPathW(static_cast<DWORD>(tempDirectory.size()), tempDirectory.data());
        THROW_HR_IF(E_FAIL, tempDirectoryLength == 0 || tempDirectoryLength >= tempDirectory.size());
        THROW_LAST_ERROR_IF(GetTempFileNameW(tempDirectory.data(), L"vsc", 0, m_path.data()) == 0);
        auto cleanupOnFailure = wil::scope_exit([&] { DeleteFileW(m_path.data()); });

        wil::unique_hfile file{CreateFileW(m_path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
        THROW_LAST_ERROR_IF(!file);

        DWORD bytesWritten{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &bytesWritten, nullptr));
        THROW_HR_IF(E_FAIL, static_cast<std::size_t>(bytesWritten) != contents.size());
        cleanupOnFailure.release();
    }

    ~TemporaryConfigFile() noexcept
    {
        if (m_path.front() != L'\0')
        {
            DeleteFileW(m_path.data());
        }
    }

    TemporaryConfigFile(const TemporaryConfigFile&) = delete;
    TemporaryConfigFile& operator=(const TemporaryConfigFile&) = delete;

    LPCWSTR Path() const noexcept
    {
        return m_path.data();
    }

private:
    std::array<wchar_t, MAX_PATH> m_path{};
};

} // namespace

namespace VirtioSoundConfigUnitTests {

class VirtioSoundConfigUnitTests
{
    WSL_TEST_CLASS(VirtioSoundConfigUnitTests)

    TEST_METHOD(SoundOptionsRemainDisabledByDefault)
    {
        const TemporaryConfigFile configFile{"[wsl2]\nvirtio=true\n"};
        const wsl::core::Config config{configFile.Path()};

        VERIFY_IS_FALSE(config.EnableVirtioSound);
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
    }

    TEST_METHOD(SoundOptionsAreBuildGatedAndLoadVirtioModuleWhenEnabled)
    {
        const TemporaryConfigFile configFile{
            "[wsl2]\n"
            "virtio=true\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint=endpoint-id\n"};
        const wsl::core::Config config{configFile.Path()};

#if defined(WSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND)
        VERIFY_IS_TRUE(config.EnableVirtioSound);
        VERIFY_IS_TRUE(config.VirtioSoundWindowsInput);
        VERIFY_ARE_EQUAL(std::wstring{L"endpoint-id"}, config.VirtioSoundCaptureEndpoint);
        const auto kernelModules = wsl::shared::string::Split(config.KernelModulesList, L',');
        VERIFY_IS_TRUE(std::find(kernelModules.begin(), kernelModules.end(), L"virtio_snd") != kernelModules.end());
#else
        VERIFY_IS_FALSE(config.EnableVirtioSound);
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
#endif
    }

    TEST_METHOD(SafeModeDisablesVirtioSoundAndWindowsInput)
    {
        const TemporaryConfigFile configFile{
            "[wsl2]\n"
            "virtio=true\n"
            "safeMode=true\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint=endpoint-id\n"};
        const wsl::core::Config config{configFile.Path()};

        VERIFY_IS_TRUE(config.EnableSafeMode);
        VERIFY_IS_FALSE(config.EnableVirtioSound);
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
        const auto kernelModules = wsl::shared::string::Split(config.KernelModulesList, L',');
        VERIFY_IS_TRUE(std::find(kernelModules.begin(), kernelModules.end(), L"virtio_snd") == kernelModules.end());
    }

    TEST_METHOD(DisablingVirtioDisablesVirtioSoundAndWindowsInput)
    {
        const TemporaryConfigFile configFile{
            "[wsl2]\n"
            "virtio=false\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint=endpoint-id\n"};
        const wsl::core::Config config{configFile.Path()};

        VERIFY_IS_FALSE(config.EnableVirtio);
        VERIFY_IS_FALSE(config.EnableVirtioSound);
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
        const auto kernelModules = wsl::shared::string::Split(config.KernelModulesList, L',');
        VERIFY_IS_TRUE(std::find(kernelModules.begin(), kernelModules.end(), L"virtio_snd") == kernelModules.end());
    }

    TEST_METHOD(DisablingVirtioSoundClearsWindowsInputConfiguration)
    {
        const TemporaryConfigFile configFile{
            "[wsl2]\n"
            "virtio=true\n"
            "[experimental]\n"
            "virtioSound=false\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint=endpoint-id\n"};
        const wsl::core::Config config{configFile.Path()};

        VERIFY_IS_FALSE(config.EnableVirtioSound);
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
        const auto kernelModules = wsl::shared::string::Split(config.KernelModulesList, L',');
        VERIFY_IS_TRUE(std::find(kernelModules.begin(), kernelModules.end(), L"virtio_snd") == kernelModules.end());
    }

    TEST_METHOD(CaptureEndpointMustBePresentForWindowsInput)
    {
        const TemporaryConfigFile configFile{
            "[wsl2]\n"
            "virtio=true\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"};

#if defined(WSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND)
        const auto loadConfig = [&]() { wsl::core::Config config{configFile.Path()}; };
        VERIFY_THROWS_SPECIFIC(loadConfig(), wil::ResultException, [](const wil::ResultException& exception) {
            return exception.GetErrorCode() == E_INVALIDARG;
        });
#else
        const wsl::core::Config config{configFile.Path()};
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
#endif
    }

    TEST_METHOD(CaptureEndpointAcceptsTheMaximumLength)
    {
        std::string configContents{
            "[wsl2]\n"
            "virtio=true\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint="};
        configContents.append(1024, 'x');
        configContents.push_back('\n');
        const TemporaryConfigFile configFile{configContents};
        const wsl::core::Config config{configFile.Path()};

#if defined(WSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND)
        VERIFY_IS_TRUE(config.VirtioSoundWindowsInput);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1024), config.VirtioSoundCaptureEndpoint.size());
#else
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
#endif
    }

    TEST_METHOD(CaptureEndpointRejectsValuesLongerThanTheMaximum)
    {
        std::string configContents{
            "[wsl2]\n"
            "virtio=true\n"
            "[experimental]\n"
            "virtioSound=true\n"
            "virtioSoundWindowsInput=true\n"
            "virtioSoundCaptureEndpoint="};
        configContents.append(1025, 'x');
        configContents.push_back('\n');
        const TemporaryConfigFile configFile{configContents};

#if defined(WSL_ENABLE_EXPERIMENTAL_VIRTIO_SOUND)
        const auto loadConfig = [&]() { wsl::core::Config config{configFile.Path()}; };
        VERIFY_THROWS_SPECIFIC(loadConfig(), wil::ResultException, [](const wil::ResultException& exception) {
            return exception.GetErrorCode() == E_INVALIDARG;
        });
#else
        const wsl::core::Config config{configFile.Path()};
        VERIFY_IS_FALSE(config.VirtioSoundWindowsInput);
        VERIFY_IS_TRUE(config.VirtioSoundCaptureEndpoint.empty());
#endif
    }
};

} // namespace VirtioSoundConfigUnitTests
