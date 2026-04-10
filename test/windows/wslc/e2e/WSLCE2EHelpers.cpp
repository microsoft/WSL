/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.cpp

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#include "precomp.h"
#include "SessionModel.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <JsonUtils.h>
#include <wslutil.h>

extern std::wstring g_testDataPath;

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

namespace {
    // Lazily compute the session storage base path.
    struct SessionStorageBasePathAccessor
    {
        operator const std::filesystem::path&() const
        {
            static const std::filesystem::path basePath =
                std::filesystem::absolute(std::filesystem::current_path() / L"wslc-cli-test-sessions");
            return basePath;
        }
    };

    static wil::com_ptr<IWSLCSessionManager> OpenSessionManager()
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());
        return sessionManager;
    }

    wil::com_ptr<IWSLCSession> CreateSession(const WSLCSessionSettings& sessionSettings, WSLCSessionFlags Flags = WSLCSessionFlagsNone)
    {
        const auto sessionManager = OpenSessionManager();
        wil::com_ptr<IWSLCSession> session;
        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        WSLCSessionState state{};
        VERIFY_SUCCEEDED(session->GetState(&state));
        VERIFY_ARE_EQUAL(state, WSLCSessionStateRunning);

        return session;
    }

    WSLCSessionSettings GetDefaultSessionSettings(LPCWSTR name, LPCWSTR storagePath, WSLCNetworkingMode networkingMode = WSLCNetworkingModeNone)
    {
        WSLCSessionSettings settings{};
        settings.DisplayName = name;
        settings.CpuCount = 4;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = storagePath;
        settings.MaximumStorageSizeMb = 4096; // 4GB.
        settings.NetworkingMode = networkingMode;
        return settings;
    }

    wil::com_ptr<IWSLCSession> CreateCustomSession(
        const std::wstring& sessionName, const std::filesystem::path& storagePath, WSLCNetworkingMode networkingMode = WSLCNetworkingModeNone)
    {
        WSLCSessionSettings sessionSettings = GetDefaultSessionSettings(sessionName.c_str(), storagePath.c_str(), networkingMode);
        return CreateSession(sessionSettings);
    }

    void CleanupCustomSession(wil::com_ptr<IWSLCSession>& session, const std::filesystem::path& storagePath)
    {
        if (session)
        {
            LOG_IF_FAILED(session->Terminate());
        }

        session.reset();

        if (!storagePath.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(storagePath, error);
            if (error)
            {
                Log::Error(std::format(L"Failed to cleanup storage path {}: {}", storagePath.wstring(), error.message()).c_str());
            }
        }
    }
} // namespace

const TestImage& AlpineTestImage()
{
    static const TestImage image{L"alpine", L"latest", std::filesystem::path{g_testDataPath} / L"alpine-latest.tar"};
    return image;
}

const TestImage& DebianTestImage()
{
    static const TestImage image{L"debian", L"latest", std::filesystem::path{g_testDataPath} / L"debian-latest.tar"};
    return image;
}

const TestImage& PythonTestImage()
{
    static const TestImage image{L"python", L"3.12-alpine", std::filesystem::path{g_testDataPath} / L"python-3_12-alpine.tar"};
    return image;
}

const TestImage& InvalidTestImage()
{
    static const TestImage image{L"mcr.microsoft.com/invalid-image", L"latest", L"INVALID_PATH"};
    return image;
}

TestSession TestSession::Create(const std::wstring& displayName, WSLCNetworkingMode networkingMode)
{
    const std::filesystem::path& basePath = SessionStorageBasePathAccessor();
    auto storagePath = basePath / displayName;
    auto session = CreateCustomSession(displayName, storagePath, networkingMode);
    return TestSession{displayName, storagePath.wstring(), std::move(session)};
}

TestSession::~TestSession()
{
    CleanupCustomSession(m_session, m_storagePath);
}

void VerifyContainerIsListed(const std::wstring& containerNameOrId, const std::wstring& status, const std::wstring& sessionName)
{
    std::wstring command = L"container list --no-trunc --all";
    if (!sessionName.empty())
    {
        command = std::format(L"container list --no-trunc --all --session {}", sessionName);
    }

    auto result = RunWslc(command);
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message = L"Container '" + containerNameOrId + L"' found in container list output but status '" +
                                         status + L"' was not found in the same line";
            VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(status), message.c_str());
            return;
        }
    }

    const std::wstring message = L"Container '" + containerNameOrId + L"' not found in container list output";
    VERIFY_FAIL(message.c_str());
}

void VerifyImageIsUsed(const TestImage& image)
{
    auto result = RunWslc(L"container list --no-trunc --all");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            return;
        }
    }

    VERIFY_FAIL(std::format(L"Image '{}' not found in container list output", image.NameAndTag()).c_str());
}

void VerifyImageIsNotUsed(const TestImage& image)
{
    auto result = RunWslc(L"container list --no-trunc --all");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            VERIFY_FAIL(std::format(L"Image '{}' found in container list output", image.NameAndTag()).c_str());
        }
    }
}

std::string GetHashId(const std::string& id, bool fullId)
{
    return wsl::windows::common::string::TruncateId(id, !fullId);
}

wslc_schema::InspectContainer InspectContainer(const std::wstring& containerName)
{
    auto result = RunWslc(std::format(L"container inspect {}", containerName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto inspectData = wsl::shared::FromJson<std::vector<wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

wslc_schema::InspectImage InspectImage(const std::wstring& imageName)
{
    auto result = RunWslc(std::format(L"image inspect {}", imageName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto inspectData = wsl::shared::FromJson<std::vector<wslc_schema::InspectImage>>(result.Stdout.value().c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    auto listResult = RunWslc(L"container list --no-trunc --all");
    listResult.Verify({.Stderr = L"", .ExitCode = 0});

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        if (line.find(containerName) != std::wstring::npos)
        {
            if (line.find(L"running") != std::wstring::npos)
            {
                auto result = RunWslc(std::format(L"container kill {}", containerName));
                // Tolerate ERROR_NOT_FOUND - container already stopped/removed
                if (result.ExitCode != 0 && (!result.Stderr.has_value() || result.Stderr.value().find(L"ERROR_NOT_FOUND") == std::wstring::npos))
                {
                    result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
                }
            }

            auto result = RunWslc(std::format(L"container remove --force {}", containerName));
            // Tolerate ERROR_NOT_FOUND - container already removed
            if (result.ExitCode != 0 && (!result.Stderr.has_value() || result.Stderr.value().find(L"ERROR_NOT_FOUND") == std::wstring::npos))
            {
                result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
            }
            break;
        }
    }
}

std::vector<wsl::windows::wslc::models::ContainerInformation> ListAllContainers()
{
    auto result = RunWslc(L"container list --all --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    return wsl::shared::FromJson<std::vector<wsl::windows::wslc::models::ContainerInformation>>(result.Stdout.value().c_str());
}

void EnsureImageContainersAreDeleted(const TestImage& image)
{
    auto containers = ListAllContainers();
    for (const auto& container : containers)
    {
        auto nameAndTag = wsl::shared::string::WideToMultiByte(image.NameAndTag());
        if (container.Image.find(nameAndTag) != std::string::npos)
        {
            auto result = RunWslc(std::format(L"container remove --force {}", container.Id));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
        }
    }
}

void EnsureImageIsDeleted(const TestImage& image)
{
    auto result = RunWslc(L"image list -q");
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            EnsureImageContainersAreDeleted(image);
            auto deleteResult = RunWslc(std::format(L"image delete --force {}", image.NameAndTag()));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

void EnsureImageIsLoaded(const TestImage& image, const std::wstring& sessionName)
{
    std::wstring listCommand = L"image list -q";
    if (!sessionName.empty())
    {
        listCommand = std::format(L"image list -q --session \"{}\"", sessionName);
    }

    auto result = RunWslc(listCommand);
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            return;
        }
    }

    // Image not found, load it
    std::wstring loadCommand = std::format(L"image load --input \"{}\"", image.Path.wstring());
    if (!sessionName.empty())
    {
        loadCommand = std::format(L"image load --input \"{}\" --session \"{}\"", image.Path.wstring(), sessionName);
    }

    auto loadResult = RunWslc(loadCommand);
    loadResult.Verify({.Stderr = L"", .ExitCode = 0});
}

void EnsureSessionIsTerminated(const std::wstring& sessionName)
{
    std::wstring targetSession = sessionName;
    if (targetSession.empty())
    {
        targetSession = std::wstring{wsl::windows::wslc::models::SessionOptions::GetDefaultSessionName()};
    }

    auto listResult = RunWslc(L"session list");
    listResult.Verify({.Stderr = L"", .ExitCode = 0});

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        // Check if the line ends with the target session name
        if (line.size() >= targetSession.size() && line.compare(line.size() - targetSession.size(), targetSession.size(), targetSession) == 0)
        {
            auto result = RunWslc(std::format(L"session terminate \"{}\"", targetSession));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

wil::com_ptr<IWSLCSession> OpenDefaultElevatedSession()
{
    wil::com_ptr<IWSLCSessionManager> sessionManager;
    VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(L"wslc-cli-admin", &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    return std::move(session);
}

std::pair<RunningWSLCContainer, std::string> StartLocalRegistry(IWSLCSession& session, const std::string& username, const std::string& password, USHORT port)
{
    EnsureImageIsLoaded({L"wslc-registry", L"latest", GetTestImagePath("wslc-registry:latest")});

    std::vector<std::string> env = {std::format("REGISTRY_HTTP_ADDR=0.0.0.0:{}", port)};
    
    if (!username.empty())
    {
        env.push_back(std::format("USERNAME={}", username));
        env.push_back(std::format("PASSWORD={}", password));
    }

    WSLCContainerLauncher launcher("wslc-registry:latest", {}, {}, env);
    launcher.SetEntrypoint({"/entrypoint.sh"});
    launcher.AddPort(port, port, AF_INET);

    auto container = launcher.Launch(session, WSLCContainerStartFlagsNone);

    auto address = std::format("127.0.0.1:{}", port);
    auto url = std::format(L"http://{}/v2/", wsl::shared::string::MultiByteToWide(address));

    int expectedCode = username.empty() ? 200 : 401;
    ExpectHttpResponse(url.c_str(), expectedCode, true);

    return {std::move(container), std::move(address)};
}

// TODO: Replace with RunWslc("image tag ...") once the 'image tag' CLI command is implemented.s
std::string TagImageForRegistry(IWSLCSession& session, const std::string& imageName, const std::string& registryAddress)
{
    auto [repo, tag] = wsl::windows::common::wslutil::ParseImage(imageName);
    const auto registryImage = std::format("{}/{}:{}", registryAddress, repo, tag.value_or("latest"));
    const auto registryRepo = std::format("{}/{}", registryAddress, repo);

    WSLCTagImageOptions tagOptions{};
    tagOptions.Image = imageName.c_str();
    tagOptions.Repo = registryRepo.c_str();
    tagOptions.Tag = tag.value_or("latest").c_str();

    VERIFY_SUCCEEDED(session.TagImage(&tagOptions));
    return registryImage;
}

} // namespace WSLCE2ETests