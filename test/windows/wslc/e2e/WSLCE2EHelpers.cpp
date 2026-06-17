/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.cpp

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#include "precomp.h"
#include "WSLCSessionDefaults.h"
#include "ImageModel.h"
#include "VolumeModel.h"
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
        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, nullptr, &session));
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
        command = std::format(L"--session {} container list --no-trunc --all", sessionName);
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

void VerifyImageIsListed(const TestImage& image)
{
    auto result = RunWslc(L"image list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto images = wsl::shared::FromJson<std::vector<wsl::windows::wslc::models::ImageInformation>>(result.Stdout.value().c_str());
    for (const auto& img : images)
    {
        if (img.Repository == wsl::shared::string::WideToMultiByte(image.Name) &&
            img.Tag == wsl::shared::string::WideToMultiByte(image.Tag))
        {
            return;
        }
    }

    VERIFY_FAIL(std::format(L"Image '{}' not found in image list output", image.NameAndTag()).c_str());
}

void VerifyVolumeIsListed(const std::wstring& volumeName)
{
    auto result = RunWslc(L"volume list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto volumes = wsl::shared::FromJson<std::vector<WSLCVolumeInformation>>(result.Stdout.value().c_str());
    for (const auto& vol : volumes)
    {
        if (vol.Name == wsl::shared::string::WideToMultiByte(volumeName))
        {
            return;
        }
    }

    VERIFY_FAIL(std::format(L"Volume '{}' not found in volume list output", volumeName).c_str());
}

void VerifyVolumeIsNotListed(const std::wstring& volumeName)
{
    auto result = RunWslc(L"volume list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto volumes = wsl::shared::FromJson<std::vector<WSLCVolumeInformation>>(result.Stdout.value().c_str());
    for (const auto& vol : volumes)
    {
        if (vol.Name == wsl::shared::string::WideToMultiByte(volumeName))
        {
            VERIFY_FAIL(std::format(L"Volume '{}' found in volume list output", volumeName).c_str());
        }
    }
}

void VerifyNetworkIsListed(const std::wstring& networkName)
{
    auto result = RunWslc(L"network list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto networks = wsl::shared::FromJson<std::vector<WSLCNetworkInformation>>(result.Stdout.value().c_str());
    for (const auto& net : networks)
    {
        if (net.Name == wsl::shared::string::WideToMultiByte(networkName))
        {
            return;
        }
    }

    VERIFY_FAIL(std::format(L"Network '{}' not found in network list output", networkName).c_str());
}

void VerifyNetworkIsNotListed(const std::wstring& networkName)
{
    auto result = RunWslc(L"network list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto networks = wsl::shared::FromJson<std::vector<WSLCNetworkInformation>>(result.Stdout.value().c_str());
    for (const auto& net : networks)
    {
        if (net.Name == wsl::shared::string::WideToMultiByte(networkName))
        {
            VERIFY_FAIL(std::format(L"Network '{}' found in network list output", networkName).c_str());
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

wslc_schema::InspectVolume InspectVolume(const std::wstring& volumeName)
{
    auto result = RunWslc(std::format(L"volume inspect {}", volumeName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto inspectData = wsl::shared::FromJson<std::vector<wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    const auto name = wsl::shared::string::WideToMultiByte(containerName);
    const auto containers = ListAllContainers();
    auto it = std::ranges::find_if(containers, [&](const auto& c) { return c.Name == name; });
    if (it == containers.end())
    {
        return;
    }

    if (it->State == WSLCContainerState::WslcContainerStateRunning)
    {
        auto result = RunWslc(std::format(L"container kill {}", containerName));
        // Tolerate WSLC_E_CONTAINER_NOT_FOUND - container already stopped/removed
        if (result.ExitCode != 0 &&
            (!result.Stderr.has_value() || result.Stderr.value().find(L"WSLC_E_CONTAINER_NOT_FOUND") == std::wstring::npos))
        {
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
        }
    }

    auto result = RunWslc(std::format(L"container remove --force {}", containerName));
    // Tolerate WSLC_E_CONTAINER_NOT_FOUND - container already removed
    if (result.ExitCode != 0 && (!result.Stderr.has_value() || result.Stderr.value().find(L"WSLC_E_CONTAINER_NOT_FOUND") == std::wstring::npos))
    {
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
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
            result.Verify({.Stdout = std::format(L"{}\r\n", container.Id), .Stderr = L"", .ExitCode = 0});
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
        listCommand = std::format(L"--session \"{}\" image list -q", sessionName);
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
        loadCommand = std::format(L"--session \"{}\" image load --input \"{}\"", sessionName, image.Path.wstring());
    }

    auto loadResult = RunWslc(loadCommand);
    loadResult.Verify({.Stderr = L"", .ExitCode = 0});
}

void EnsureSessionIsTerminated(const std::wstring& sessionName)
{
    std::wstring targetSession = sessionName;
    if (targetSession.empty())
    {
        auto isElevated = wsl::windows::common::security::IsTokenElevated(wil::open_current_access_token(TOKEN_QUERY).get());
        auto baseName = isElevated ? wsl::windows::wslc::DefaultAdminSessionName : wsl::windows::wslc::DefaultSessionName;

        wchar_t username[256 + 1] = {};
        DWORD usernameLen = ARRAYSIZE(username);
        THROW_IF_WIN32_BOOL_FALSE(GetUserNameW(username, &usernameLen));

        targetSession = std::format(L"{}-{}", baseName, username);
    }

    auto listResult = RunWslc(L"system session list");
    listResult.Verify({.Stderr = L"", .ExitCode = 0});

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        // Check if the line ends with the target session name
        if (line.size() >= targetSession.size() && line.compare(line.size() - targetSession.size(), targetSession.size(), targetSession) == 0)
        {
            auto result = RunWslc(std::format(L"system session terminate \"{}\"", targetSession));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

void EnsureVolumeDoesNotExist(const std::wstring& volumeName)
{
    auto result = RunWslc(L"volume list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto volumes = wsl::shared::FromJson<std::vector<WSLCVolumeInformation>>(result.Stdout.value().c_str());
    for (const auto& vol : volumes)
    {
        if (vol.Name == wsl::shared::string::WideToMultiByte(volumeName))
        {
            auto deleteResult = RunWslc(std::format(L"volume rm {}", volumeName));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

void EnsureNetworkDoesNotExist(const std::wstring& networkName)
{
    auto result = RunWslc(L"network list --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto networks = wsl::shared::FromJson<std::vector<WSLCNetworkInformation>>(result.Stdout.value().c_str());
    for (const auto& net : networks)
    {
        if (net.Name == wsl::shared::string::WideToMultiByte(networkName))
        {
            auto deleteResult = RunWslc(std::format(L"network rm {}", networkName));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

wslc_schema::Network InspectNetwork(const std::wstring& networkName)
{
    auto result = RunWslc(std::format(L"network inspect {}", networkName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto inspectData = wsl::shared::FromJson<std::vector<wslc_schema::Network>>(result.Stdout.value().c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

wil::com_ptr<IWSLCSession> OpenDefaultElevatedSession()
{
    // Ensure the default elevated session exists before opening it via COM.
    RunWslcAndVerify(L"container list", {.Stderr = L"", .ExitCode = 0});

    wil::com_ptr<IWSLCSessionManager> sessionManager;
    VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLCSession> session;
    VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(nullptr, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    return std::move(session);
}

std::wstring TagImageForRegistry(const std::wstring& imageName, const std::wstring& registryAddress)
{
    auto registryImage = std::format(L"{}/{}", registryAddress, imageName);
    RunWslcAndVerify(std::format(L"image tag {} {}", imageName, registryImage), {.ExitCode = 0});
    return registryImage;
}

void WriteTestFile(const std::filesystem::path& filePath, const std::vector<std::string>& lines)
{
    std::ofstream file(filePath, std::ios::out | std::ios::trunc | std::ios::binary);
    VERIFY_IS_TRUE(file.is_open());
    for (const auto& line : lines)
    {
        file << line << "\n";
    }

    VERIFY_IS_TRUE(file.good());
}

std::wstring GetPythonHttpServerScript(uint16_t port)
{
    return std::format(L"python3 -u -m http.server {}", port);
}

std::wstring GetPythonUdpEchoServerScript(uint16_t port)
{
    // Inline Python UDP echo server: echoes each received datagram back uppercased, forever.
    return std::format(
        L"python3 -c \"import socket;"
        L"s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);"
        L"s.bind(('0.0.0.0',{}));"
        L"[s.sendto(d.upper(),a) for d,a in iter(lambda:s.recvfrom(1024),0)]\"",
        port);
}

std::string SendUdpAndReceive(uint16_t hostPort, const std::string& payload, const std::string& expectedReply, int family)
{
    SOCKADDR_INET addr{};
    addr.si_family = static_cast<ADDRESS_FAMILY>(family);
    INETADDR_SETLOOPBACK(reinterpret_cast<PSOCKADDR>(&addr));
    SS_PORT(&addr) = htons(hostPort);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do
    {
        wil::unique_socket sock{::socket(family, SOCK_DGRAM, IPPROTO_UDP)};
        THROW_LAST_ERROR_IF(!sock);

        DWORD timeout = 1000;
        THROW_LAST_ERROR_IF(
            setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR);

        if (sendto(sock.get(), payload.data(), static_cast<int>(payload.size()), 0, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) !=
            SOCKET_ERROR)
        {
            char buf[1024];
            const int received = recvfrom(sock.get(), buf, sizeof(buf), 0, nullptr, nullptr);
            if (received != SOCKET_ERROR && received > 0 && std::string(buf, received) == expectedReply)
            {
                return std::string(buf, received);
            }
        }
    } while (std::chrono::steady_clock::now() < deadline);

    VERIFY_FAIL(L"Timed out waiting for expected UDP echo reply from container");
    return {};
}

namespace {

    void WaitForTtySize(const WSLCInteractiveSession& session, SHORT columns, SHORT rows)
    {
        try
        {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    const std::string data = session.GetStdoutData();
                    THROW_HR_IF(E_ABORT, data.find(std::format("{} {}\r\n", rows, columns)) == std::string::npos);
                },
                std::chrono::milliseconds(200),
                std::chrono::seconds(60));
        }
        catch (...)
        {
            const std::string data = session.GetStdoutData();
            VERIFY_FAIL(std::format(
                            L"Timed out waiting for tty resize. Captured pseudoconsole output: \"{}\"",
                            wsl::shared::string::MultiByteToWide(EscapeString(data)))
                            .c_str());
        }
    }

} // namespace

void VerifyPseudoConsoleTtySize(WSLCInteractiveSession& session, SHORT columns, SHORT rows)
{
    constexpr SHORT resizedColumns = 100;
    constexpr SHORT resizedRows = 37;
    VERIFY_IS_TRUE(columns != resizedColumns || rows != resizedRows, L"Resized tty size must differ from the initial size");

    WaitForTtySize(session, columns, rows);

    session.ResizePseudoConsole(resizedColumns, resizedRows);
    WaitForTtySize(session, resizedColumns, resizedRows);

    session.Terminate();
}
} // namespace WSLCE2ETests
