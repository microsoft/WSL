/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSdkWinRtTests.cpp

Abstract:

    This file contains test cases for the WSLC SDK WinRT projection.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslcsdk.h"
#include "WslcsdkPrivate.h"
#include "WSLCContainerLauncher.h"
#include "wslutil.h"
#include "wslc/e2e/WSLCE2EHelpers.h"

#include "winrt/Session.h"
#include "winrt/Helpers.h"

#include <winrt/Microsoft.WSL.Containers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;

namespace WSLCSDK = winrt::Microsoft::WSL::Containers;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

#define VERIFY_THROWS_HR(operation, expectedHr) \
    VERIFY_THROWS_SPECIFIC( \
        operation, winrt::hresult_error, [&](winrt::hresult_error const& e) { return e.code().value == expectedHr; })

#define IGNORE_ERRORS(operation) \
    try \
    { \
        operation; \
    } \
    CATCH_LOG()
#define SCOPE_CLEANUP(operation) wil::scope_exit([&]() { IGNORE_ERRORS(operation) })
#define DELETE_CONTAINER_ON_SCOPE_EXIT(container) SCOPE_CLEANUP(container.Delete(WSLCSDK::DeleteContainerFlags::Force))
#define DELETE_IMAGE_ON_SCOPE_EXIT(imageName) SCOPE_CLEANUP(m_defaultSession.DeleteImage(imageName))

struct ProcessOutput
{
    uint32_t ExitCode;
    std::wstring StandardOutput;
    std::wstring StandardError;
};

std::wstring ReadStream(IInputStream const& stream)
{
    std::wstring output;
    DataReader reader{stream};
    reader.UnicodeEncoding(winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8);
    uint32_t bytesRead;
    do
    {
        bytesRead = reader.LoadAsync(1024).get();
        output += reader.ReadString(bytesRead).c_str();
    } while (bytesRead > 0);
    return output;
}

class WslcSdkWinRtTests
{
    WSLC_TEST_CLASS(WslcSdkWinRtTests)

    std::filesystem::path m_storagePath;
    WSLCSDK::Session m_defaultSession{nullptr};

    static inline constexpr auto c_testSessionName = L"wslc-winrt-test";

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void StartProcessAndWaitForExit(WSLCSDK::Process const& process, std::chrono::milliseconds timeout = 2min)
    {
        std::promise<void> promise;
        auto autoRevoker = process.Exited(winrt::auto_revoke, [&](int32_t) { promise.set_value(); });
        process.Start();
        VERIFY_ARE_EQUAL(promise.get_future().wait_for(timeout), std::future_status::ready);
    }

    void StartContainerAndWaitForInitProcessExit(WSLCSDK::Container const& container, std::chrono::milliseconds timeout = 2min)
    {
        auto initProcess = container.InitProcess();
        std::promise<void> promise;
        auto autoRevoker = initProcess.Exited(winrt::auto_revoke, [&](int32_t) { promise.set_value(); });
        container.Start();
        VERIFY_ARE_EQUAL(promise.get_future().wait_for(timeout), std::future_status::ready);
    }

    ProcessOutput GetProcessOutput(WSLCSDK::Process const& process)
    {
        ProcessOutput output;
        output.ExitCode = process.ExitCode();
        output.StandardOutput = ReadStream(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput));
        output.StandardError = ReadStream(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardError));

        return output;
    }

    struct RunContainerOptions
    {
        std::vector<winrt::hstring> cmdLine = {};
        WSLCSDK::ContainerFlags flags = WSLCSDK::ContainerFlags::None;
        std::optional<winrt::hstring> name = std::nullopt;
        std::chrono::milliseconds timeout = 2min;
        std::optional<WSLCSDK::ContainerNetworkingMode> networkingMode = std::nullopt;
    };

    // Creates and starts a one-shot container, waits for the init process to
    // exit, and returns the exit code.
    ProcessOutput RunContainerAndWaitForExit(winrt::hstring imageName, RunContainerOptions options = {})
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        if (!options.cmdLine.empty())
        {
            procSettings.CmdLine(winrt::single_threaded_vector(std::move(options.cmdLine)));
        }

        procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Stream);

        auto containerSettings = WSLCSDK::ContainerSettings(imageName);
        containerSettings.InitProcess(procSettings);
        containerSettings.Flags(options.flags);

        if (options.name)
        {
            containerSettings.Name(options.name.value());
        }

        if (options.networkingMode)
        {
            containerSettings.NetworkingMode(options.networkingMode.value());
        }

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        StartContainerAndWaitForInitProcessExit(container, options.timeout);
        auto output = GetProcessOutput(container.InitProcess());

        IGNORE_ERRORS(container.Delete(WSLCSDK::DeleteContainerFlags::Force));

        return output;
    }

    bool HasImage(winrt::hstring const& imageName)
    {
        auto images = m_defaultSession.Images();
        return std::any_of(images.begin(), images.end(), [&](auto const& img) { return img.Name() == imageName; });
    }

    // Starts a local wslc-registry container using host-mode networking.
    // Host networking is not exposed by the WinRT projection, so this helper
    // uses WSLCContainerLauncher with the raw session handle.
    std::pair<wsl::windows::common::RunningWSLCContainer, std::string> StartLocalRegistry(
        const std::string& username = {}, const std::string& password = {}, uint16_t port = 5000)
    {
        // Get the IWSLCSession COM object from the SDK session handle and delegate to the shared helper.
        auto& comSession = *reinterpret_cast<WslcSessionImpl*>(WSLCSDK::implementation::GetHandle(m_defaultSession))->session;
        return WSLCE2ETests::StartLocalRegistry(comSession, username, password, port);
    }

    // Tags and pushes an image to a local registry via the SDK APIs.
    void PushImageToRegistry(const std::string& repo, const std::string& tag, const std::string& registryAddress, const std::string& registryAuth)
    {
        const auto imageName = winrt::to_hstring(std::format("{}:{}", repo, tag));
        const auto registryRepo = winrt::to_hstring(std::format("{}/{}", registryAddress, repo));
        const auto registryImage = winrt::to_hstring(std::format("{}/{}:{}", registryAddress, repo, tag));

        VERIFY_IS_TRUE(HasImage(imageName));

        m_defaultSession.TagImage(WSLCSDK::TagImageOptions(imageName, registryRepo, winrt::to_hstring(tag)));

        // Ensures the registry-prefixed tag is removed after the push.
        auto cleanup = DELETE_IMAGE_ON_SCOPE_EXIT(registryImage);

        m_defaultSession.PushImageAsync(WSLCSDK::PushImageOptions(registryImage, winrt::to_hstring(registryAuth))).get();
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        WSADATA wsaData;
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &wsaData));

        winrt::init_apartment();

        // Use the same storage path as WSLC runtime tests to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";

        // Build session settings using the WinRT API
        auto settings = WSLCSDK::SessionSettings(c_testSessionName, m_storagePath.wstring());
        settings.CpuCount(4);
        settings.MemoryMB(2048);
        settings.Timeout(std::chrono::duration_cast<TimeSpan>(30s));
        settings.VhdRequirements(WSLCSDK::VhdOptions(L"", 4096ull * 1024 * 1024, WSLCSDK::VhdType::Dynamic));

        m_defaultSession = WSLCSDK::Session(settings);
        m_defaultSession.Start();

        // Pull images required by the tests (no-op if already present).
        for (const auto* imageName : {"debian:latest", "python:3.12-alpine", "hello-world:latest", "wslc-registry:latest"})
        {
            const auto imagePath = GetTestImagePath(imageName);
            m_defaultSession.LoadImageAsync(imagePath.wstring()).get();
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_defaultSession)
        {
            m_defaultSession.Terminate();
            m_defaultSession = nullptr;
        }

        // Preserve the VHD in fast-run mode so subsequent runs skip image pulling.
        if (!g_fastTestRun && !m_storagePath.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(m_storagePath, error);
            if (error)
            {
                LogError("Failed to cleanup storage path %ws: %hs", m_storagePath.c_str(), error.message().c_str());
            }
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // Session tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(CreateSession)
    {
        const std::filesystem::path extraStorage = m_storagePath / "wslc-winrt-extra-session-storage";

        auto settings = WSLCSDK::SessionSettings(L"wslc-winrt-extra-session", extraStorage.wstring());
        settings.CpuCount(2);
        settings.MemoryMB(1024);
        settings.Timeout(std::chrono::duration_cast<TimeSpan>(30s));
        settings.VhdRequirements(WSLCSDK::VhdOptions(L"", 1024ull * 1024 * 1024, WSLCSDK::VhdType::Dynamic));

        // Positive: Creation must succeed with valid settings.
        {
            auto session = WSLCSDK::Session(settings);
            VERIFY_IS_NOT_NULL(session);
        }

        // Negative: Must throw if used before Start()
        {
            auto session = WSLCSDK::Session(settings);
            VERIFY_THROWS_HR(std::ignore = session.Images(), E_ILLEGAL_METHOD_CALL);
        }

        // Positive: Starting the session must succeed.
        {
            auto session = WSLCSDK::Session(settings);
            VERIFY_NO_THROW(session.Start());
        }

        // Negative: Null settings must fail.
        {
            VERIFY_THROWS_HR(WSLCSDK::Session(WSLCSDK::SessionSettings{nullptr}), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(TerminationHandler)
    {
        // Positive: Terminating the session must trigger a graceful shutdown and fire the event
        std::promise<WSLCSDK::SessionTerminationReason> promise;

        const std::filesystem::path extraStorage = m_storagePath / "wslc-winrt-termh-storage";

        auto settings = WSLCSDK::SessionSettings(L"wslc-winrt-termh", extraStorage.wstring());
        settings.Timeout(std::chrono::duration_cast<TimeSpan>(30s));

        auto session = WSLCSDK::Session(settings);
        session.Terminated([&](WSLCSDK::SessionTerminationReason reason) { promise.set_value(reason); });

        session.Start();

        session.Terminate();

        auto future = promise.get_future();
        VERIFY_ARE_EQUAL(future.wait_for(30s), std::future_status::ready);
        VERIFY_ARE_EQUAL(future.get(), WSLCSDK::SessionTerminationReason::Shutdown);
    }

    // -----------------------------------------------------------------------
    // Image tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(ImageList)
    {
        // Session has images pre-loaded - list must return at least one entry.
        const auto images = m_defaultSession.Images();
        VERIFY_IS_TRUE(images.Size() >= 1);

        // At least one image must be non-empty
        bool foundNonEmpty = false;
        for (auto const& img : images)
        {
            if (!img.Name().empty() && img.SizeBytes() != 0)
            {
                foundNonEmpty = true;
                break;
            }
        }
        VERIFY_IS_TRUE(foundNonEmpty);
    }

    WSLC_TEST_METHOD(LoadImage)
    {
        // Positive: load a saved image tar and verify the image can be run
        {
            // Remove the image if it already exists
            IGNORE_ERRORS(m_defaultSession.DeleteImage(L"hello-world:latest"));

            const auto imageTar = GetTestImagePath("hello-world:latest");

            // Positive: load from file path.
            VERIFY_NO_THROW(m_defaultSession.LoadImageAsync(imageTar.wstring()).get());

            // Verify the loaded image is usable
            VERIFY_IS_TRUE(HasImage(L"hello-world:latest"));
            auto output = RunContainerAndWaitForExit(L"hello-world:latest", {});
            VERIFY_ARE_EQUAL(output.ExitCode, 0);
            VERIFY_IS_TRUE(output.StandardOutput.find(L"Hello from Docker!") != std::string::npos);
        }

        // Negative: empty path must fail
        {
            VERIFY_THROWS_HR(m_defaultSession.LoadImageAsync(L"").get(), E_INVALIDARG);
        }

        // Negative: non-existent path must fail.
        {
            VERIFY_THROWS_HR(m_defaultSession.LoadImageAsync(L"C:\\bogus\\image.tar").get(), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        }
    }

    WSLC_TEST_METHOD(ImportImage)
    {
        const auto exportedImageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        constexpr auto c_importedImageName = L"my-hello-world-winrt:test";

        IGNORE_ERRORS(m_defaultSession.DeleteImage(c_importedImageName));

        // Positive: import an exported image tar via path.
        {
            auto cleanup = DELETE_IMAGE_ON_SCOPE_EXIT(c_importedImageName);

            VERIFY_NO_THROW(m_defaultSession.ImportImageAsync(exportedImageTar.wstring(), c_importedImageName).get());

            VERIFY_IS_TRUE(HasImage(c_importedImageName));

            auto output = RunContainerAndWaitForExit(c_importedImageName, {.cmdLine = {L"/hello"}});
            VERIFY_ARE_EQUAL(output.ExitCode, 0);
            VERIFY_IS_TRUE(output.StandardOutput.find(L"Hello from Docker!") != std::string::npos);
        }

        // Negative: empty path must fail.
        {
            VERIFY_THROWS_HR(m_defaultSession.ImportImageAsync(L"", c_importedImageName).get(), E_INVALIDARG);
        }

        // Negative: empty image name must fail.
        {
            VERIFY_THROWS_HR(m_defaultSession.ImportImageAsync(exportedImageTar.wstring(), L"").get(), E_INVALIDARG);
        }

        // Negative: non-tar file must fail.
        {
            std::filesystem::path pathToSelf = wil::QueryFullProcessImageNameW<std::wstring>(GetCurrentProcess());
            VERIFY_THROWS_HR(m_defaultSession.ImportImageAsync(pathToSelf.wstring(), L"import-self:test").get(), E_FAIL);
        }
    }

    WSLC_TEST_METHOD(ImageDelete)
    {
        VERIFY_IS_TRUE(HasImage(L"hello-world:latest"));

        // Positive: delete an existing image.
        {
            m_defaultSession.DeleteImage(L"hello-world:latest");
            VERIFY_IS_FALSE(HasImage(L"hello-world:latest"));

            // Reload for subsequent tests.
            const auto imageTar = GetTestImagePath("hello-world:latest");
            m_defaultSession.LoadImageAsync(imageTar.wstring()).get();
        }

        // Negative: non-existent image name must throw.
        {
            VERIFY_THROWS_HR(m_defaultSession.DeleteImage(L"nonexistent:no-such-tag"), WSLC_E_IMAGE_NOT_FOUND);
        }
    }

    // -----------------------------------------------------------------------
    // Container lifecycle tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(CreateContainer)
    {
        // Positive: stdout is captured correctly.
        {
            auto output = RunContainerAndWaitForExit(L"debian:latest", {.cmdLine = {L"/bin/echo", L"OK"}});
            VERIFY_ARE_EQUAL(output.ExitCode, 0);
            VERIFY_ARE_EQUAL(output.StandardOutput, L"OK\n");
            VERIFY_ARE_EQUAL(output.StandardError, L"");
        }

        // Positive: stdout and stderr are routed independently.
        {
            auto output =
                RunContainerAndWaitForExit(L"debian:latest", {.cmdLine = {L"/bin/sh", L"-c", L"echo stdout && echo stderr >&2"}});
            VERIFY_ARE_EQUAL(output.ExitCode, 0);
            VERIFY_ARE_EQUAL(output.StandardOutput, L"stdout\n");
            VERIFY_ARE_EQUAL(output.StandardError, L"stderr\n");
        }

        // Negative: creating a container with a non-existent image fails at CreateContainer.
        {
            WSLCSDK::ContainerSettings containerSettings{L"invalid-image:notfound"};
            VERIFY_THROWS_HR(m_defaultSession.CreateContainer(containerSettings), WSLC_E_IMAGE_NOT_FOUND);
        }

        // Negative: an empty image name is rejected.
        {
            VERIFY_THROWS_HR(WSLCSDK::ContainerSettings{L""}, E_INVALIDARG);
        }

        // Verify that a null settings pointer is rejected.
        {
            VERIFY_THROWS_HR(m_defaultSession.CreateContainer(nullptr), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(ContainerGetId)
    {
        auto container = m_defaultSession.CreateContainer(WSLCSDK::ContainerSettings(L"debian:latest"));
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        const auto id = container.Id();
        VERIFY_IS_FALSE(id.empty());
        // Container ID is a 64-character lowercase hex string.
        VERIFY_ARE_EQUAL(id.size(), 64u);
    }

    WSLC_TEST_METHOD(ContainerGetState)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        // State after creation: Created.
        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Created);

        container.Start();

        // State while running: Running.
        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Running);

        container.Stop(WSLCSDK::Signal::SIGKILL, TimeSpan::zero());

        // State after stop: Exited.
        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Exited);
    }

    WSLC_TEST_METHOD(ContainerStopAndDelete)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"999"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        VERIFY_NO_THROW(container.Start());

        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Running);

        VERIFY_NO_THROW(container.Stop(WSLCSDK::Signal::SIGKILL, TimeSpan::zero()));
        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Exited);

        VERIFY_NO_THROW(container.Delete(WSLCSDK::DeleteContainerFlags::None));
        VERIFY_ARE_EQUAL(container.State(), WSLCSDK::ContainerState::Deleted);
    }

    WSLC_TEST_METHOD(ProcessIOHandles)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"echo STDOUT_TOKEN; echo STDERR_TOKEN >&2"}));
        procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Stream);

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        auto initProcess = container.InitProcess();

        std::promise<void> promise;
        auto autoRevoker = initProcess.Exited(winrt::auto_revoke, [&](int32_t) { promise.set_value(); });

        container.Start();

        auto stdoutStream = initProcess.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput);
        auto stderrStream = initProcess.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardError);

        // Verify that each handle can only be acquired once.
        {
            VERIFY_THROWS_HR(initProcess.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
            VERIFY_THROWS_HR(initProcess.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardError), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        VERIFY_ARE_EQUAL(promise.get_future().wait_for(1min), std::future_status::ready);

        VERIFY_ARE_EQUAL(ReadStream(stdoutStream), L"STDOUT_TOKEN\n");
        VERIFY_ARE_EQUAL(ReadStream(stderrStream), L"STDERR_TOKEN\n");
    }

    WSLC_TEST_METHOD(ContainerNetworkingMode)
    {
        // BRIDGED: eth0 interface must be present.
        {
            auto output = RunContainerAndWaitForExit(
                L"debian:latest",
                {.cmdLine = {L"/bin/sh", L"-c", L"[ -d /sys/class/net/eth0 ] && echo 'HAS_ETH0' || echo 'NO_ETH0'"},
                 .flags = WSLCSDK::ContainerFlags::None,
                 .networkingMode = WSLCSDK::ContainerNetworkingMode::Bridged});

            VERIFY_ARE_EQUAL(output.StandardOutput, L"HAS_ETH0\n");
        }

        // NONE: eth0 interface must not be present.
        {
            auto output = RunContainerAndWaitForExit(
                L"debian:latest",
                {.cmdLine = {L"/bin/sh", L"-c", L"[ -d /sys/class/net/eth0 ] && echo 'HAS_ETH0' || echo 'NO_ETH0'"},
                 .flags = WSLCSDK::ContainerFlags::None,
                 .networkingMode = WSLCSDK::ContainerNetworkingMode::None});

            VERIFY_ARE_EQUAL(output.StandardOutput, L"NO_ETH0\n");
        }

        // Invalid networking mode must fail.
        {
            WSLCSDK::ContainerSettings containerSettings{L"debian:latest"};
            VERIFY_THROWS_HR(containerSettings.NetworkingMode(static_cast<WSLCSDK::ContainerNetworkingMode>(99)), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerPortMapping)
    {
        // Negative: port mappings with None networking mode must fail at Start.
        {
            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.NetworkingMode(WSLCSDK::ContainerNetworkingMode::None);
            containerSettings.PortMappings(winrt::single_threaded_vector<WSLCSDK::ContainerPortMapping>(
                {WSLCSDK::ContainerPortMapping(12342, 8000, WSLCSDK::PortProtocol::TCP)}));

            VERIFY_THROWS_HR(m_defaultSession.CreateContainer(containerSettings), E_INVALIDARG);
        }

        // Functional: BRIDGED networking with port mapping; HTTP server must be reachable.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"python3", L"-m", L"http.server", L"8000"}));
            procSettings.EnvironmentVariables(
                winrt::single_threaded_map(std::map<winrt::hstring, winrt::hstring>{{L"PYTHONUNBUFFERED", L"1"}}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"python:3.12-alpine");
            containerSettings.InitProcess(procSettings);
            containerSettings.NetworkingMode(WSLCSDK::ContainerNetworkingMode::Bridged);
            containerSettings.PortMappings(winrt::single_threaded_vector<WSLCSDK::ContainerPortMapping>(
                {WSLCSDK::ContainerPortMapping(12341, 8000, WSLCSDK::PortProtocol::TCP)}));

            auto container = m_defaultSession.CreateContainer(containerSettings);
            container.Start();

            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            ExpectHttpResponse(L"http://127.0.0.1:12341", 200, true);
        }

        // Functional: port mapping with explicit IPv4 WindowsAddress.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"python3", L"-m", L"http.server", L"8000"}));
            procSettings.EnvironmentVariables(
                winrt::single_threaded_map(std::map<winrt::hstring, winrt::hstring>{{L"PYTHONUNBUFFERED", L"1"}}));

            auto portMapping = WSLCSDK::ContainerPortMapping(12343, 8000, WSLCSDK::PortProtocol::TCP);
            portMapping.WindowsAddress(winrt::Windows::Networking::HostName(L"127.0.0.1"));

            auto containerSettings = WSLCSDK::ContainerSettings(L"python:3.12-alpine");
            containerSettings.InitProcess(procSettings);
            containerSettings.NetworkingMode(WSLCSDK::ContainerNetworkingMode::Bridged);
            containerSettings.PortMappings(winrt::single_threaded_vector<WSLCSDK::ContainerPortMapping>({portMapping}));

            auto container = m_defaultSession.CreateContainer(containerSettings);
            container.Start();

            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            ExpectHttpResponse(L"http://127.0.0.1:12343", 200, true);
        }

        // Functional: port mapping with explicit IPv6 WindowsAddress.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(
                winrt::single_threaded_vector<winrt::hstring>({L"python3", L"-m", L"http.server", L"--bind", L"::", L"8000"}));
            procSettings.EnvironmentVariables(
                winrt::single_threaded_map(std::map<winrt::hstring, winrt::hstring>{{L"PYTHONUNBUFFERED", L"1"}}));

            auto portMapping = WSLCSDK::ContainerPortMapping(12344, 8000, WSLCSDK::PortProtocol::TCP);
            portMapping.WindowsAddress(winrt::Windows::Networking::HostName(L"::1"));

            auto containerSettings = WSLCSDK::ContainerSettings(L"python:3.12-alpine");
            containerSettings.InitProcess(procSettings);
            containerSettings.NetworkingMode(WSLCSDK::ContainerNetworkingMode::Bridged);
            containerSettings.PortMappings(winrt::single_threaded_vector<WSLCSDK::ContainerPortMapping>({portMapping}));

            auto container = m_defaultSession.CreateContainer(containerSettings);
            container.Start();

            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            ExpectHttpResponse(L"http://[::1]:12344", 200, true);
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeUnit)
    {
        const auto currentDirectory = std::filesystem::current_path().wstring();

        // Negative: non-absolute Windows path must fail at CreateContainer.
        VERIFY_THROWS_HR(
            {
                auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
                containerSettings.Volumes(winrt::single_threaded_vector<WSLCSDK::ContainerVolume>(
                    {WSLCSDK::ContainerVolume(L"relative", L"/mnt/path", false)}));
                m_defaultSession.CreateContainer(containerSettings);
            },
            E_INVALIDARG);

        // Negative: non-absolute container path must fail at CreateContainer.
        VERIFY_THROWS_HR(
            {
                auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
                containerSettings.Volumes(winrt::single_threaded_vector<WSLCSDK::ContainerVolume>(
                    {WSLCSDK::ContainerVolume(currentDirectory, L"./mnt/path", false)}));
                m_defaultSession.CreateContainer(containerSettings);
            },
            E_INVALIDARG);

        // Positive: absolute paths must succeed.
        {
            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.Volumes(winrt::single_threaded_vector<WSLCSDK::ContainerVolume>(
                {WSLCSDK::ContainerVolume(currentDirectory, L"/mnt/path", false)}));
            auto container = m_defaultSession.CreateContainer(containerSettings);
            container.Delete(WSLCSDK::DeleteContainerFlags::None);
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeFunctional)
    {
        const auto hostRwDir = std::filesystem::current_path() / "wslc-winrt-test-vol-rw";
        const auto hostRoDir = std::filesystem::current_path() / "wslc-winrt-test-vol-ro";
        std::filesystem::create_directories(hostRwDir);
        std::filesystem::create_directories(hostRoDir);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostRwDir, ec);
            std::filesystem::remove_all(hostRoDir, ec);
        });

        std::ofstream{hostRwDir / "hello.txt"} << "hello-rw";
        std::ofstream{hostRoDir / "hello.txt"} << "hello-ro";

        // Container script exits 0 if all checks pass:
        //   1. RW mount is readable (hello-rw).
        //   2. RO mount is readable (hello-ro).
        //   3. Writing to RW mount succeeds.
        //   4. Writing to RO mount fails (! touch).
        constexpr auto c_script =
            "test \"$(cat /mnt/rw/hello.txt)\" = hello-rw && "
            "test \"$(cat /mnt/ro/hello.txt)\" = hello-ro && "
            "echo container-write > /mnt/rw/written.txt && "
            "! touch /mnt/ro/probe 2>/dev/null";

        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", winrt::to_hstring(c_script)}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);
        containerSettings.Volumes(winrt::single_threaded_vector<WSLCSDK::ContainerVolume>({
            WSLCSDK::ContainerVolume(hostRwDir.wstring(), L"/mnt/rw", false),
            WSLCSDK::ContainerVolume(hostRoDir.wstring(), L"/mnt/ro", true),
        }));

        auto container = m_defaultSession.CreateContainer(containerSettings);
        StartContainerAndWaitForInitProcessExit(container);

        VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
        container.Delete(WSLCSDK::DeleteContainerFlags::Force);

        // Verify the file written by the container is visible on the host.
        std::ifstream written(hostRwDir / "written.txt");
        VERIFY_IS_TRUE(written.is_open());
        std::string writtenContent((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
        VERIFY_ARE_EQUAL(writtenContent, "container-write\n");
    }

    WSLC_TEST_METHOD(ContainerInspect)
    {
        auto container = m_defaultSession.CreateContainer(WSLCSDK::ContainerSettings(L"debian:latest"));
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        const auto inspectJson = container.Inspect();
        VERIFY_IS_FALSE(inspectJson.empty());

        const auto id = container.Id();
        VERIFY_IS_FALSE(id.empty());

        // The inspect JSON must contain the container ID.
        VERIFY_IS_TRUE(winrt::to_string(inspectJson).find(winrt::to_string(id)) != std::string::npos);

        container.Delete(WSLCSDK::DeleteContainerFlags::None);
        cleanup.release();
    }

    WSLC_TEST_METHOD(ContainerExec)
    {
        // Start a long-running container so we can exec into it.
        auto initProcSettings = WSLCSDK::ProcessSettings();
        initProcSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(initProcSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        // Positive: exec a command that exits 0.
        {
            auto execSettings = WSLCSDK::ProcessSettings();
            execSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/true"}));

            auto execProcess = container.CreateProcess(execSettings);
            StartProcessAndWaitForExit(execProcess);
            VERIFY_ARE_EQUAL(execProcess.ExitCode(), 0);
        }

        // Negative: no command line must fail.
        VERIFY_THROWS_HR(container.CreateProcess(WSLCSDK::ProcessSettings()).Start(), E_INVALIDARG);
    }

    WSLC_TEST_METHOD(ContainerHostName)
    {
        // Unit: setting a hostname must succeed.
        {
            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.HostName(L"unit-test-host");
        }

        // Functional: container process should see the configured hostname.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(
                winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"test \"$(hostname)\" = my-test-host"}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);
            containerSettings.HostName(L"my-test-host");

            auto container = m_defaultSession.CreateContainer(containerSettings);
            StartContainerAndWaitForInitProcessExit(container);
            VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
            container.Delete(WSLCSDK::DeleteContainerFlags::Force);
        }
    }

    WSLC_TEST_METHOD(ContainerDomainName)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"test \"$(domainname)\" = test.local"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);
        containerSettings.DomainName(L"test.local");

        auto container = m_defaultSession.CreateContainer(containerSettings);
        StartContainerAndWaitForInitProcessExit(container);
        VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
        container.Delete(WSLCSDK::DeleteContainerFlags::Force);
    }

    // -----------------------------------------------------------------------
    // Process tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(ProcessEnvVariables)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"test \"$MY_TEST_VAR\" = hello-from-test"}));
        procSettings.EnvironmentVariables(
            winrt::single_threaded_map(std::map<winrt::hstring, winrt::hstring>{{L"MY_TEST_VAR", L"hello-from-test"}}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        StartContainerAndWaitForInitProcessExit(container);
        VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
        container.Delete(WSLCSDK::DeleteContainerFlags::Force);
    }

    WSLC_TEST_METHOD(ProcessSignal)
    {
        // Negative: Signal() before Start() must throw.
        {
            auto container = m_defaultSession.CreateContainer(WSLCSDK::ContainerSettings(L"debian:latest"));
            VERIFY_THROWS_HR(container.InitProcess().Signal(WSLCSDK::Signal::SIGKILL), E_ILLEGAL_METHOD_CALL);
        }

        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto process = container.InitProcess();

        std::promise<void> promise;
        auto autoRevoker = process.Exited(winrt::auto_revoke, [&](int32_t) { promise.set_value(); });

        container.Start();
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        VERIFY_ARE_EQUAL(process.State(), WSLCSDK::ProcessState::Running);

        process.Signal(WSLCSDK::Signal::SIGKILL);

        VERIFY_ARE_EQUAL(promise.get_future().wait_for(2min), std::future_status::ready);

        const auto state = process.State();
        VERIFY_IS_TRUE(state == WSLCSDK::ProcessState::Signalled || state == WSLCSDK::ProcessState::Exited);
    }

    WSLC_TEST_METHOD(ProcessGetPid)
    {
        // Negative: Pid() before Start() must throw.
        {
            auto container = m_defaultSession.CreateContainer(WSLCSDK::ContainerSettings(L"debian:latest"));
            VERIFY_THROWS_HR(std::ignore = container.InitProcess().Pid(), E_ILLEGAL_METHOD_CALL);
        }

        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        auto process = container.InitProcess();
        VERIFY_IS_TRUE(process.Pid() > 0);
    }

    WSLC_TEST_METHOD(ProcessGetExitCode)
    {
        auto runAndGetExitCode = [&](int code) -> int32_t {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(
                winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", winrt::to_hstring(std::format("exit {}", code))}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            StartContainerAndWaitForInitProcessExit(container);
            auto exitCode = container.InitProcess().ExitCode();

            container.Delete(WSLCSDK::DeleteContainerFlags::Force);
            return exitCode;
        };

        VERIFY_ARE_EQUAL(runAndGetExitCode(0), 0);
        VERIFY_ARE_EQUAL(runAndGetExitCode(42), 42);

        // Negative: querying ExitCode while process is still running must throw.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            container.Start();

            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            auto process = container.InitProcess();
            VERIFY_ARE_EQUAL(process.State(), WSLCSDK::ProcessState::Running);

            VERIFY_THROWS_HR(std::ignore = process.ExitCode(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    WSLC_TEST_METHOD(ProcessGetState)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        auto process = container.InitProcess();

        // State while running.
        VERIFY_ARE_EQUAL(process.State(), WSLCSDK::ProcessState::Running);

        // Querying ExitCode while running must throw ERROR_INVALID_STATE.
        VERIFY_THROWS_HR(std::ignore = process.ExitCode(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

        // Register the Exited event.
        std::promise<int32_t> exitPromise;
        auto token = process.Exited([&](int32_t code) { exitPromise.set_value(code); });
        auto cleanupToken = wil::scope_exit([&]() { process.Exited(token); });

        process.Signal(WSLCSDK::Signal::SIGKILL);

        // The Exited event must fire after the signal.
        auto future = exitPromise.get_future();
        VERIFY_ARE_EQUAL(future.wait_for(30s), std::future_status::ready);

        const auto state = process.State();
        VERIFY_IS_TRUE(state == WSLCSDK::ProcessState::Signalled || state == WSLCSDK::ProcessState::Exited);
    }

    WSLC_TEST_METHOD(ProcessWorkingDirectory)
    {
        // Functional: container should see the configured working directory.
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"test \"$(pwd)\" = /tmp"}));
        procSettings.WorkingDirectory(L"/tmp");

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        StartContainerAndWaitForInitProcessExit(container);
        VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
        container.Delete(WSLCSDK::DeleteContainerFlags::Force);
    }

    // -----------------------------------------------------------------------
    // Service tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(GetVersion)
    {
        auto version = WSLCSDK::WslcService::GetVersion();
        VERIFY_IS_TRUE(version.Major() > 0 || version.Minor() > 0 || version.Revision() > 0);
    }

    WSLC_TEST_METHOD(GetMissingComponents)
    {
        const auto missing = WSLCSDK::WslcService::GetMissingComponents();
        VERIFY_ARE_EQUAL(missing, WSLCSDK::ComponentFlags::None);
    }

    WSLC_TEST_METHOD(InstallWithDependencies)
    {
        WSLCSDK::WslcService::InstallWithDependenciesAsync().get();
        VERIFY_ARE_EQUAL(WSLCSDK::WslcService::GetMissingComponents(), WSLCSDK::ComponentFlags::None);
    }

    // -----------------------------------------------------------------------
    // Process IO event tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(ProcessIoEventsUnit)
    {
        // Negative: registering OutputReceived/ErrorReceived without OutputMode::Event must throw.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"1"}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            auto process = container.InitProcess();
            VERIFY_THROWS_HR(process.OutputReceived([](winrt::array_view<uint8_t const>) {}), E_ILLEGAL_METHOD_CALL);
            VERIFY_THROWS_HR(process.ErrorReceived([](winrt::array_view<uint8_t const>) {}), E_ILLEGAL_METHOD_CALL);

            // GetOutputStream requires OutputMode::Stream — must throw with Discard mode (even after Start).
            container.Start();
            VERIFY_THROWS_HR(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput), E_ILLEGAL_METHOD_CALL);
        }

        // Positive: with OutputMode::Event, registering and revoking event handlers must succeed.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"1"}));
            procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            auto process = container.InitProcess();

            auto stdoutToken = process.OutputReceived([](winrt::array_view<uint8_t const>) {});
            auto stderrToken = process.ErrorReceived([](winrt::array_view<uint8_t const>) {});
            auto exitToken = process.Exited([](int32_t) {});

            process.OutputReceived(stdoutToken);
            process.ErrorReceived(stderrToken);
            process.Exited(exitToken);

            // GetOutputStream throws when OutputMode is Event.
            container.Start();
            VERIFY_THROWS_HR(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput), E_ILLEGAL_METHOD_CALL);
        }

        // Negative: OutputReceived/ErrorReceived with OutputMode::Stream must throw.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"1"}));
            procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Stream);

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            auto process = container.InitProcess();
            VERIFY_THROWS_HR(process.OutputReceived([](winrt::array_view<uint8_t const>) {}), E_ILLEGAL_METHOD_CALL);
            VERIFY_THROWS_HR(process.ErrorReceived([](winrt::array_view<uint8_t const>) {}), E_ILLEGAL_METHOD_CALL);
        }
    }

    WSLC_TEST_METHOD(ProcessIoEventsInitProcess)
    {
        std::string stdoutData, stderrData;

        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"echo STDOUT && echo STDERR >&2"}));
        procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto process = container.InitProcess();

        process.OutputReceived([&](winrt::array_view<uint8_t const> data) {
            stdoutData.append(reinterpret_cast<const char*>(data.data()), data.size());
        });
        process.ErrorReceived([&](winrt::array_view<uint8_t const> data) {
            stderrData.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        // Start: claims IO handles and starts the IOCallback pump thread.
        StartContainerAndWaitForInitProcessExit(container);

        VERIFY_ARE_EQUAL(stdoutData, "STDOUT\n");
        VERIFY_ARE_EQUAL(stderrData, "STDERR\n");
    }

    WSLC_TEST_METHOD(ProcessIoEventsExecProcess)
    {
        // Long-running init process to keep the container alive.
        auto initProcSettings = WSLCSDK::ProcessSettings();
        initProcSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(initProcSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        std::string stdoutData, stderrData;

        auto execProcSettings = WSLCSDK::ProcessSettings();
        execProcSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"echo EXEC_OUT && echo EXEC_ERR >&2"}));
        execProcSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

        auto execProcess = container.CreateProcess(execProcSettings);

        execProcess.OutputReceived([&](winrt::array_view<uint8_t const> data) {
            stdoutData.append(reinterpret_cast<const char*>(data.data()), data.size());
        });
        execProcess.ErrorReceived([&](winrt::array_view<uint8_t const> data) {
            stderrData.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        StartProcessAndWaitForExit(execProcess);

        VERIFY_ARE_EQUAL(stdoutData, "EXEC_OUT\n");
        VERIFY_ARE_EQUAL(stderrData, "EXEC_ERR\n");
    }

    WSLC_TEST_METHOD(ProcessIoEventsHandleExclusion)
    {
        // Register an OutputReceived handler only. The IOCallback acquires ALL pipe handles
        // (draining uncallbacked streams to prevent deadlock), so both stdout and stderr
        // handles are consumed and neither can be obtained via GetOutputStream.
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"99"}));
        procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        auto process = container.InitProcess();
        process.OutputReceived([](winrt::array_view<uint8_t const>) {});

        container.Start();

        // stdout handle was consumed by the OutputReceived handler — must not be obtainable.
        VERIFY_THROWS_HR(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardOutput), E_ILLEGAL_METHOD_CALL);

        // stderr handle was also consumed in order to drain it despite not having a handler.
        VERIFY_THROWS_HR(process.GetOutputStream(WSLCSDK::ProcessOutputHandle::StandardError), E_ILLEGAL_METHOD_CALL);
    }

    WSLC_TEST_METHOD(ProcessIoEventsExitCallback)
    {
        // Verify the Exited event fires with the correct exit code after IO has been flushed.
        auto RunAndCaptureExit = [&](int exitCodeArg) -> std::pair<int32_t, std::string> {
            std::string stdoutData;
            std::promise<int32_t> exitPromise;

            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>(
                {L"/bin/sh", L"-c", winrt::hstring(std::format(L"echo HELLO && exit {}", exitCodeArg))}));
            procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);

            auto container = m_defaultSession.CreateContainer(containerSettings);
            auto process = container.InitProcess();

            process.OutputReceived([&](winrt::array_view<uint8_t const> data) {
                stdoutData.append(reinterpret_cast<const char*>(data.data()), data.size());
            });
            process.Exited([&](int32_t code) { exitPromise.set_value(code); });

            container.Start();

            auto future = exitPromise.get_future();
            VERIFY_ARE_EQUAL(future.wait_for(60s), std::future_status::ready);

            return {future.get(), stdoutData};
        };

        // Exit 0: Exited event must fire with code 0; IO must have been delivered first.
        {
            auto [exitCode, output] = RunAndCaptureExit(0);
            VERIFY_ARE_EQUAL(exitCode, 0);
            VERIFY_ARE_EQUAL(output, "HELLO\n");
        }

        // Non-zero exit: Exited event must report the correct code.
        {
            auto [exitCode, output] = RunAndCaptureExit(42);
            VERIFY_ARE_EQUAL(exitCode, 42);
            VERIFY_ARE_EQUAL(output, "HELLO\n");
        }
    }

    WSLC_TEST_METHOD(ProcessIoEventsCancelOnRelease)
    {
        // Verify that releasing the process handle while an exec'd process is still running
        // and writing IO cancels the event pump:
        //   - No IO events arrive after the handle is released.
        //   - Exited is never invoked (cancellation suppresses it).

        // Long-running init process to keep the container alive.
        auto initProcSettings = WSLCSDK::ProcessSettings();
        initProcSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"999"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(initProcSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        std::atomic<int> callbackCount{0};
        std::atomic<bool> exitFired{false};

        auto execProcSettings = WSLCSDK::ProcessSettings();
        execProcSettings.CmdLine(
            winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"while true; do echo LINE; sleep 0.05; done"}));
        execProcSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

        auto execProcess = container.CreateProcess(execProcSettings);

        execProcess.OutputReceived([&](winrt::array_view<uint8_t const>) { callbackCount.fetch_add(1); });
        execProcess.Exited([&](int32_t) { exitFired.store(true); });

        execProcess.Start();

        // Wait for events to start arriving.
        Sleep(500);
        VERIFY_IS_TRUE(callbackCount.load() > 0);

        // Release the exec process handle while it is still running and writing.
        execProcess = nullptr;

        const int countAtRelease = callbackCount.load();

        // Exited must not have fired: cancellation suppresses it.
        VERIFY_IS_FALSE(exitFired.load());

        // No further events after release.
        Sleep(200);
        VERIFY_ARE_EQUAL(callbackCount.load(), countAtRelease);
        VERIFY_IS_FALSE(exitFired.load());
    }

    WSLC_TEST_METHOD(ProcessIoEventsLargeOutput)
    {
        // Generate ~1 MiB of stdout via: dd if=/dev/zero bs=1024 count=1024 | base64
        // 1,048,576 zero bytes → base64 output is 1,398,104 bytes.
        static constexpr size_t c_expectedBytes = 1'398'104;

        std::string stdoutData;
        stdoutData.reserve(c_expectedBytes + 4096);

        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>(
            {L"/bin/sh", L"-c", L"dd if=/dev/zero bs=1024 count=1024 2>/dev/null | base64 -w 0"}));
        procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        auto process = container.InitProcess();

        process.OutputReceived([&](winrt::array_view<uint8_t const> data) {
            stdoutData.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        StartContainerAndWaitForInitProcessExit(container);

        VERIFY_ARE_EQUAL(stdoutData.size(), c_expectedBytes);
    }

    // -----------------------------------------------------------------------
    // Storage tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(SessionCreateVhd)
    {
        constexpr auto c_volumeName = L"wslc-winrt-test-data-vol";
        constexpr uint64_t c_vhdSizeBytes = 1ull * 1024 * 1024 * 1024; // 1 GiB

        const std::filesystem::path vhdSessionStorage = m_storagePath / "wslc-winrt-vhd-test-storage";
        IGNORE_ERRORS(std::filesystem::remove_all(vhdSessionStorage));
        auto cleanup = SCOPE_CLEANUP(std::filesystem::remove_all(vhdSessionStorage));

        // Create a dedicated session so that volume creation does not affect the shared default session.
        auto settings = WSLCSDK::SessionSettings(L"wslc-winrt-vhd-test", vhdSessionStorage.wstring());
        settings.Timeout(std::chrono::duration_cast<TimeSpan>(30s));
        settings.VhdRequirements(WSLCSDK::VhdOptions(L"", 4096ull * 1024 * 1024, WSLCSDK::VhdType::Dynamic));

        auto session = WSLCSDK::Session(settings);
        session.Start();

        // Load debian.
        const auto debianTar = GetTestImagePath("debian:latest");
        session.LoadImageAsync(debianTar.wstring()).get();

        // Positive: create a named VHD volume.
        session.CreateVhdVolume(WSLCSDK::VhdOptions(c_volumeName, c_vhdSizeBytes, WSLCSDK::VhdType::Dynamic));

        // The backing VHD file must exist on disk.
        const auto expectedVhdPath = vhdSessionStorage / "volumes" / (std::wstring(c_volumeName) + L".vhdx");
        VERIFY_IS_TRUE(std::filesystem::exists(expectedVhdPath));

        // Positive: write a marker via a container that mounts the named volume.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>(
                {L"/bin/sh", L"-c", L"echo wslc-winrt-vhd-test > /data/marker.txt"}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);
            containerSettings.NamedVolumes(winrt::single_threaded_vector<WSLCSDK::ContainerNamedVolume>(
                {WSLCSDK::ContainerNamedVolume(c_volumeName, L"/data", false)}));

            auto container = session.CreateContainer(containerSettings);
            StartContainerAndWaitForInitProcessExit(container);
            VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
            container.Delete(WSLCSDK::DeleteContainerFlags::Force);
        }

        // Positive: read back the marker in a second container (read-only mount).
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>(
                {L"/bin/sh", L"-c", L"test \"$(cat /data/marker.txt)\" = wslc-winrt-vhd-test"}));

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);
            containerSettings.NamedVolumes(winrt::single_threaded_vector<WSLCSDK::ContainerNamedVolume>(
                {WSLCSDK::ContainerNamedVolume(c_volumeName, L"/data", true)}));

            auto container = session.CreateContainer(containerSettings);
            StartContainerAndWaitForInitProcessExit(container);
            VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
            container.Delete(WSLCSDK::DeleteContainerFlags::Force);
        }

        // Positive: delete the volume.
        session.DeleteVhdVolume(c_volumeName);
        VERIFY_IS_FALSE(std::filesystem::exists(expectedVhdPath));

        // Negative: zero size must fail.
        VERIFY_THROWS_HR(session.CreateVhdVolume(WSLCSDK::VhdOptions(c_volumeName, 0, WSLCSDK::VhdType::Dynamic)), E_INVALIDARG);

        // Positive: fixed-allocation VHD; on-disk file size must be >= SizeBytes.
        {
            constexpr auto c_fixedVolumeName = L"wslc-sdk-vhd-fixed";
            constexpr auto c_fixedSizeBytes = 64ull * _1MB;
            VERIFY_NO_THROW(session.CreateVhdVolume(WSLCSDK::VhdOptions(c_fixedVolumeName, c_fixedSizeBytes, WSLCSDK::VhdType::Fixed)));

            auto deleteVolume = SCOPE_CLEANUP(session.DeleteVhdVolume(c_fixedVolumeName));

            std::filesystem::path expectedVhdPath = vhdSessionStorage / L"volumes" / (std::wstring(c_fixedVolumeName) + L".vhdx");
            VERIFY_IS_TRUE(std::filesystem::exists(expectedVhdPath));
            VERIFY_IS_GREATER_THAN_OR_EQUAL(std::filesystem::file_size(expectedVhdPath), c_fixedSizeBytes);
        }

        // Positive: SetOwner() bakes uid/gid into the volume root inode at mkfs time.
        // Verify by stat-ing the mount inside a container.
        {
            constexpr auto c_ownedVolumeName = L"wslc-sdk-vhd-owned";
            auto vhdOptions = WSLCSDK::VhdOptions(c_ownedVolumeName, c_vhdSizeBytes, WSLCSDK::VhdType::Dynamic);
            vhdOptions.SetOwner(65534, 65534); // nobody:nogroup
            session.CreateVhdVolume(vhdOptions);

            auto deleteVolume = SCOPE_CLEANUP(session.DeleteVhdVolume(c_ownedVolumeName));

            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/usr/bin/stat", L"-c", L"%u %g", L"/data"}));
            procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Stream);

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);
            containerSettings.NamedVolumes(winrt::single_threaded_vector<WSLCSDK::ContainerNamedVolume>(
                {WSLCSDK::ContainerNamedVolume(c_ownedVolumeName, L"/data", false)}));

            auto container = session.CreateContainer(containerSettings);
            StartContainerAndWaitForInitProcessExit(container);
            auto output = GetProcessOutput(container.InitProcess());
            VERIFY_ARE_EQUAL(container.InitProcess().ExitCode(), 0);
            VERIFY_ARE_EQUAL(output.StandardOutput, L"65534 65534\n");
            container.Delete(WSLCSDK::DeleteContainerFlags::Force);
        }
    }

    // -----------------------------------------------------------------------
    // Authentication / registry tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(AuthenticateTests)
    {
        constexpr auto c_username = "wslctest";
        constexpr auto c_password = "password";

        auto [registryContainer, registryAddress] = StartLocalRegistry(c_username, c_password);

        const auto serverUri = Uri(winrt::to_hstring(std::format("http://{}", registryAddress)));

        // Negative: wrong password must fail.
        VERIFY_THROWS_HR(m_defaultSession.Authenticate(serverUri, winrt::to_hstring(c_username), L"wrong-password"), E_FAIL);

        // Positive: correct credentials
        VERIFY_NO_THROW(m_defaultSession.Authenticate(serverUri, winrt::to_hstring(c_username), winrt::to_hstring(c_password)));

        const auto xRegistryAuth = wsl::windows::common::wslutil::BuildRegistryAuthHeader(c_username, c_password);
        PushImageToRegistry("hello-world", "latest", registryAddress, xRegistryAuth);

        const auto image = winrt::to_hstring(std::format("{}/hello-world:latest", registryAddress));

        // Positive: pulling with correct credentials must succeed.
        {
            auto opts = WSLCSDK::PullImageOptions(image);
            opts.RegistryAuth(winrt::to_hstring(xRegistryAuth));
            m_defaultSession.PullImageAsync(opts).get();
            VERIFY_IS_TRUE(HasImage(image));
        }

        // Negative: pulling without credentials must fail.
        {
            VERIFY_THROWS_HR(m_defaultSession.PullImageAsync(WSLCSDK::PullImageOptions(image)).get(), E_FAIL);
        }

        // Negative: pulling with bad credentials must fail.
        {
            auto badAuth = wsl::windows::common::wslutil::BuildRegistryAuthHeader(c_username, "wrong");
            auto opts = WSLCSDK::PullImageOptions(image);
            opts.RegistryAuth(winrt::to_hstring(badAuth));
            VERIFY_THROWS_HR(m_defaultSession.PullImageAsync(opts).get(), E_FAIL);
        }
    }

    WSLC_TEST_METHOD(PullImage)
    {
        auto [registryContainer, registryAddress] = StartLocalRegistry();
        const auto xRegistryAuth = wsl::windows::common::wslutil::BuildRegistryAuthHeader("", "");

        {
            PushImageToRegistry("hello-world", "latest", registryAddress, xRegistryAuth);

            const auto image = winrt::to_hstring(std::format("{}/hello-world:latest", registryAddress));

            // Delete the image locally so the pull is a real network pull.
            IGNORE_ERRORS(m_defaultSession.DeleteImage(image));

            // Positive: pull from the local registry.
            m_defaultSession.PullImageAsync(WSLCSDK::PullImageOptions(image)).get();
            VERIFY_IS_TRUE(HasImage(image));

            // Verify the pulled image is runnable.
            auto output = RunContainerAndWaitForExit(image, {});
            VERIFY_ARE_EQUAL(output.ExitCode, 0);
        }

        // Negative: image that does not exist in the registry.
        {
            const auto missing = winrt::to_hstring(std::format("{}/does-not-exist", registryAddress));
            auto opts = WSLCSDK::PullImageOptions(missing);
            opts.RegistryAuth(winrt::to_hstring(xRegistryAuth));
            VERIFY_THROWS_HR(m_defaultSession.PullImageAsync(opts).get(), static_cast<HRESULT>(WSLC_E_IMAGE_NOT_FOUND));
        }

        // Negative: empty URI must fail.
        VERIFY_THROWS_HR(m_defaultSession.PullImageAsync(WSLCSDK::PullImageOptions(L"")).get(), E_INVALIDARG);
    }

    WSLC_TEST_METHOD(PushImage)
    {
        auto [registryContainer, registryAddress] = StartLocalRegistry();
        const auto xRegistryAuth = wsl::windows::common::wslutil::BuildRegistryAuthHeader("", "");

        // Positive: push an existing image to the local registry.
        PushImageToRegistry("hello-world", "latest", registryAddress, xRegistryAuth);

        // Negative: pushing a non-existent image must fail.
        VERIFY_THROWS_HR(
            m_defaultSession.PushImageAsync(WSLCSDK::PushImageOptions(L"does-not-exist", winrt::to_hstring(xRegistryAuth))).get(), E_FAIL);

        // Negative: empty image name must fail.
        VERIFY_THROWS_HR(m_defaultSession.PushImageAsync(WSLCSDK::PushImageOptions(L"", winrt::to_hstring(xRegistryAuth))).get(), E_INVALIDARG);
    }

    WSLC_TEST_METHOD(TagImage)
    {
        // Positive: tag an existing image.
        m_defaultSession.TagImage(WSLCSDK::TagImageOptions(L"debian:latest", L"debian", L"winrt-sdk-test-tag"));
        VERIFY_IS_TRUE(HasImage(L"debian:winrt-sdk-test-tag"));

        auto cleanup = DELETE_IMAGE_ON_SCOPE_EXIT(L"debian:winrt-sdk-test-tag");

        // Negative: empty image name must fail.
        VERIFY_THROWS_HR(m_defaultSession.TagImage(WSLCSDK::TagImageOptions(L"", L"debian", L"test")), E_INVALIDARG);

        // Negative: empty repository must fail.
        VERIFY_THROWS_HR(m_defaultSession.TagImage(WSLCSDK::TagImageOptions(L"debian:latest", L"", L"test")), E_INVALIDARG);

        // Negative: empty tag must fail.
        VERIFY_THROWS_HR(m_defaultSession.TagImage(WSLCSDK::TagImageOptions(L"debian:latest", L"debian", L"")), E_INVALIDARG);
    }

    // -----------------------------------------------------------------------
    // Negative / edge-case tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(ExecOnStoppedContainer)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"10"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);

        // Wait for the short-lived init process to exit
        StartContainerAndWaitForInitProcessExit(container);

        // The init process has now exited. Attempting to exec on a stopped container must fail.
        auto execSettings = WSLCSDK::ProcessSettings();
        execSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/echo", L"should-fail"}));

        VERIFY_THROWS_HR(container.CreateProcess(execSettings).Start(), static_cast<HRESULT>(WSLC_E_CONTAINER_NOT_RUNNING));
    }

    WSLC_TEST_METHOD(DuplicateContainerName)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"10"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);
        containerSettings.Name(L"duplicate-name-test-winrt");

        auto container1 = m_defaultSession.CreateContainer(containerSettings);
        container1.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container1);

        // Creating a second container with the same name must fail.
        VERIFY_THROWS_HR(m_defaultSession.CreateContainer(containerSettings), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    }

    WSLC_TEST_METHOD(DeleteRunningContainerWithoutForce)
    {
        auto procSettings = WSLCSDK::ProcessSettings();
        procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>({L"/bin/sleep", L"10"}));

        auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
        containerSettings.InitProcess(procSettings);

        auto container = m_defaultSession.CreateContainer(containerSettings);
        container.Start();

        auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

        // Deleting a running container without Force must fail.
        VERIFY_THROWS_HR(container.Delete(WSLCSDK::DeleteContainerFlags::None), static_cast<HRESULT>(WSLC_E_CONTAINER_IS_RUNNING));
    }

    WSLC_TEST_METHOD(DeleteNonExistentImage)
    {
        VERIFY_THROWS_HR(m_defaultSession.DeleteImage(L"nonexistent-image:this-tag-does-not-exist"), static_cast<HRESULT>(WSLC_E_IMAGE_NOT_FOUND));
    }

    WSLC_TEST_METHOD(PullInvalidImageUri)
    {
        VERIFY_THROWS_HR(m_defaultSession.PullImageAsync(WSLCSDK::PullImageOptions(L"///invalid-registry-url///")).get(), E_INVALIDARG);
    }

    WSLC_TEST_METHOD(ContainerGpu)
    {
        // Negative: creating a GPU container on a session without GPU support must fail.
        {
            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.Flags(WSLCSDK::ContainerFlags::EnableGpu);

            VERIFY_THROWS_HR(m_defaultSession.CreateContainer(containerSettings).Start(), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }

        // Create a GPU-enabled session.
        const std::filesystem::path gpuStorage = m_storagePath / "wslc-winrt-gpu-session-storage";
        auto cleanupStorage = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            std::error_code error;
            std::filesystem::remove_all(gpuStorage, error);
        });

        auto settings = WSLCSDK::SessionSettings(L"wslc-winrt-gpu-test", gpuStorage.wstring());
        settings.FeatureFlags(WSLCSDK::SessionFeatureFlags::EnableGpu);
        settings.VhdRequirements(WSLCSDK::VhdOptions(L"", 4096ull * 1024 * 1024, WSLCSDK::VhdType::Dynamic));

        auto gpuSession = WSLCSDK::Session(settings);
        gpuSession.Start();

        const auto debianTar = GetTestImagePath("debian:latest");
        gpuSession.LoadImageAsync(debianTar.wstring()).get();

        // Positive: /dev/dxg must be available with read/write permissions, and the dynamic linker must be configured to resolve
        // the WSL GPU libraries inside a GPU container.
        {
            auto procSettings = WSLCSDK::ProcessSettings();
            procSettings.CmdLine(winrt::single_threaded_vector<winrt::hstring>(
                {L"/bin/sh",
                 L"-c",
                 L"test -c /dev/dxg && test -r /dev/dxg && test -w /dev/dxg && cat /etc/ld.so.conf.d/ld.wsl.conf"}));
            procSettings.OutputMode(WSLCSDK::ProcessOutputMode::Stream);

            auto containerSettings = WSLCSDK::ContainerSettings(L"debian:latest");
            containerSettings.InitProcess(procSettings);
            containerSettings.Flags(WSLCSDK::ContainerFlags::EnableGpu);

            auto container = gpuSession.CreateContainer(containerSettings);
            auto cleanup = DELETE_CONTAINER_ON_SCOPE_EXIT(container);

            StartContainerAndWaitForInitProcessExit(container);
            auto output = GetProcessOutput(container.InitProcess());

            VERIFY_ARE_EQUAL(output.StandardOutput, L"/usr/lib/wsl/lib\n");
        }

        gpuSession.Terminate();
    }
};
