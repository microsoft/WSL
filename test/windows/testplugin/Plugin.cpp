/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plugin.cpp

Abstract:

    This file contains a test plugin.

--*/

#include "precomp.h"
#include "WslPluginApi.h"
#include "wslc_schema.h"

#include "PluginTests.h"

using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::relay;
using namespace wsl::shared::string;
using namespace std::chrono_literals;

std::ofstream g_logfile;
std::optional<GUID> g_distroGuid;

const WSLPluginAPIV1* g_api = nullptr;
PluginTestType g_testType = PluginTestType::Invalid;

std::optional<uint32_t> g_previousInitPid;

std::vector<char> ReadFromSocket(SOCKET socket)
{
    // Simplified error handling for the sake of the demo.
    int result = 0;
    int offset = 0;

    std::vector<char> content(1024);
    while ((result = recv(socket, content.data() + offset, 1024, 0)) > 0)
    {
        offset += result;
        content.resize(offset + 1024);
    }

    content.resize(offset);
    return content;
}

HRESULT OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    g_logfile << "VM created (settings->CustomConfigurationFlags=" << Settings->CustomConfigurationFlags << ")" << std::endl;

    if (g_testType == PluginTestType::FailToStartVm)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::FailToStartVmWithPluginErrorMessage)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        g_api->PluginError(L"Plugin error message");
        return E_UNEXPECTED;
    }
    else if (WI_IsFlagSet(Settings->CustomConfigurationFlags, WSLUserConfigurationCustomKernel))
    {
        g_logfile << "OnVmStarted: E_ACCESSDENIED" << std::endl;
        return E_ACCESSDENIED;
    }
    else if (g_testType == PluginTestType::Success)
    {
        // Get the current module's directory
        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        auto mountSource = modulePath.parent_path().wstring();

        // Mount the folder with the linux binary in the vm
        RETURN_IF_FAILED(
            g_api->MountFolder(Session->SessionId, mountSource.c_str(), L"/test-plugin/deep/folder", true, L"test-plugin-mount"));

        g_logfile << "Folder mounted (" << wsl::shared::string::WideToMultiByte(mountSource) << " -> /test-plugin)" << std::endl;

        // Create a file with dummy content
        std::ofstream file(mountSource + L"\\test-file.txt");
        if (!file || !(file << "OK"))
        {
            g_logfile << "Failed to open test-file.txt in: " << wsl::shared::string::WideToMultiByte(mountSource) << std::endl;
            return E_ABORT;
        }

        file.close();

        // Launch the process
        std::vector<const char*> arguments = {"/bin/cat", "/test-plugin/deep/folder/test-file.txt", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Read the socket output
        auto output = ReadFromSocket(socket.get());
        if (output != std::vector<char>{'O', 'K'})
        {
            g_logfile << "Got unexpected output from bash" << std::endl;
            return E_ABORT;
        }
    }
    else if (g_testType == PluginTestType::ApiErrors)
    {
        auto result = g_api->MountFolder(Session->SessionId, L"C:\\DoesNotExit", L"/dummy", true, L"test-plugin-mount");
        if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            g_logfile << "Unexpected error for MountFolder(): " << result << std::endl;
            return E_ABORT;
        }

        wil::unique_socket socket;
        std::vector<const char*> arguments = {"/bin/does-no-exist", nullptr};
        result = g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket);
        if (result != E_FAIL)
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        result = g_api->ExecuteBinary(0xcafe, arguments[0], arguments.data(), &socket);
        if (result != RPC_E_DISCONNECTED)
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        // Call PluginError asynchronously to verify that we handle this properly.

        std::thread thread{[Session]() {
            const auto result = g_api->PluginError(L"Dummy");

            if (result != E_ILLEGAL_METHOD_CALL)
            {
                g_logfile << "Unexpected error for async PluginError(): " << result << std::endl;
            }
        }};

        thread.join();

        g_logfile << "API error tests passed" << std::endl;
    }
    else if (g_testType == PluginTestType::ErrorMessageStartVm)
    {
        auto result = g_api->PluginError(L"StartVm plugin error message");
        if (FAILED(result))
        {
            g_logfile << "Unexpected error from PluginError(): " << result << std::endl;
        }
        g_logfile << "OnVmStarted: E_FAIL" << std::endl;
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::GetUsername)
    {
        try
        {
            auto info = wil::get_token_information<TOKEN_USER>(Session->UserToken);

            DWORD size{};
            DWORD domainSize{};
            SID_NAME_USE use{};
            LookupAccountSid(nullptr, info->User.Sid, nullptr, &size, nullptr, &domainSize, &use);

            THROW_HR_IF(E_UNEXPECTED, size < 1);
            std::wstring user(size - 1, '\0');
            std::wstring domain(domainSize - 1, '\0');

            THROW_IF_WIN32_BOOL_FALSE(LookupAccountSid(nullptr, info->User.Sid, user.data(), &size, domain.data(), &domainSize, &use));

            g_logfile << "Username: " << wsl::shared::string::WideToMultiByte(domain) << "\\"
                      << wsl::shared::string::WideToMultiByte(user) << std::endl;
        }
        catch (...)
        {
            g_logfile << "OnVmStarted: get_token_information failed: " << wil::ResultFromCaughtException() << std::endl;
            return E_FAIL;
        }

        return S_OK;
    }

    return S_OK;
}

HRESULT OnVmStopping(const WSLSessionInformation* Session)
{
    g_logfile << "VM Stopping" << std::endl;

    if (g_testType == PluginTestType::FailToStopVm)
    {
        g_logfile << "OnVmStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistroStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    g_logfile << "Distribution started, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", PidNs=" << Distribution->PidNamespace << ", InitPid=" << Distribution->InitPid
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToStartDistro)
    {
        g_logfile << "OnDistroStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId)
    {
        if (g_distroGuid.has_value())
        {
            if (IsEqualGUID(g_distroGuid.value(), Distribution->Id))
            {
                g_logfile << "OnDistroStarted: received same GUID" << std::endl;
            }
            else
            {
                g_logfile << "OnDistroStarted: received different GUID" << std::endl;
            }
        }
        else
        {
            g_distroGuid = Distribution->Id;
        }
    }
    else if (g_testType == PluginTestType::ErrorMessageStartDistro)
    {
        g_logfile << "OnDistroStarted: E_FAIL" << std::endl;
        g_api->PluginError(L"StartDistro plugin error message");
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::InitPidIsDifferent)
    {
        if (g_previousInitPid.has_value())
        {
            if (g_previousInitPid.value() != Distribution->InitPid)
            {
                g_logfile << "Init's pid is different (" << Distribution->InitPid << " ! = " << g_previousInitPid.value() << ")" << std::endl;
            }
            else
            {
                g_logfile << "Init's pid did not change (" << g_previousInitPid.value() << ")" << std::endl;
                return E_FAIL;
            }
        }
        else
        {
            g_previousInitPid = Distribution->InitPid;
        }
    }
    else if (g_testType == PluginTestType::RunDistroCommand)
    {
        // Launch a process
        std::vector<const char*> arguments = {"/bin/sh", "-c", "cat /etc/issue.net", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Validate that the process actually ran inside the distro.
        auto output = ReadFromSocket(socket.get());
        const auto expected = "Debian GNU/Linux 13\n";
        if (std::string(output.begin(), output.end()) != expected)
        {
            g_logfile << "Got unexpected output from bash: " << std::string(output.begin(), output.end())
                      << ", expected: " << expected << std::endl;
            return E_ABORT;
        }

        // Verify that failure to launch a process behaves properly.
        arguments = {"/does-not-exist"};
        g_logfile << "Failed process launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket)
                  << std::endl;

        const GUID guid{};
        g_logfile << "Invalid distro launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &guid, arguments[0], arguments.data(), &socket) << std::endl;
    }

    return S_OK;
}

HRESULT OnDistroStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    g_logfile << "Distribution Stopping, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", PidNs=" << Distribution->PidNamespace << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToStopDistro)
    {
        g_logfile << "OnDistroStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId && g_distroGuid.has_value())
    {
        if (!IsEqualGUID(g_distroGuid.value(), Distribution->Id))
        {
            g_logfile << "OnDistroStarted: received different GUID" << std::endl;
        }
    }

    return S_OK;
}

HRESULT OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution registered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionRegistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution unregistered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionUnregistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnWslcSessionCreated(const WSLCSessionInformation* Session)
try
{
    g_logfile << "WSLC Session created, name=" << wsl::shared::string::WideToMultiByte(Session->DisplayName) << ", id=" << Session->SessionId
              << ", pid=" << Session->ApplicationPid << ", token=" << (Session->UserToken != nullptr ? "set" : "null")
              << ", sid=" << (Session->UserSid != nullptr ? "set" : "null") << std::endl;

    if (g_testType == PluginTestType::WslcSessionRejected)
    {
        g_logfile << "OnWslcSessionCreated: ERROR_ACCESS_DENIED" << std::endl;
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    if (g_testType == PluginTestType::WslcSuccess)
    {
        // Helper: run a command in the root namespace and return (status, stdout, stderr).
        auto runCommand = [&](const char* cmd,
                              const std::optional<std::string>& input = {},
                              std::vector<const char*> env = {}) -> std::tuple<int, std::string, std::string> {
            std::vector<const char*> arguments = {"/bin/sh", "-c", cmd, nullptr};
            WSLCProcessHandle process = nullptr;
            THROW_IF_FAILED(g_api->WSLCCreateProcess(
                Session->SessionId, arguments[0], arguments.data(), env.empty() ? nullptr : env.data(), &process, nullptr));
            auto releaseProcess = wil::scope_exit([&]() { g_api->WSLCReleaseProcess(process); });

            wil::unique_handle stdinHandle;
            wil::unique_handle stdoutHandle;
            wil::unique_handle stderrHandle;
            wil::unique_handle exitEvent;
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStdin, &stdinHandle));
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStdout, &stdoutHandle));
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStderr, &stderrHandle));
            THROW_IF_FAILED(g_api->WSLCProcessGetExitEvent(process, &exitEvent));

            std::string out;
            std::string err;

            MultiHandleWait io;
            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stdoutHandle), [&out](const auto& span) { out.append(span.begin(), span.end()); }));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stderrHandle), [&err](const auto& span) { err.append(span.begin(), span.end()); }));

            io.AddHandle(std::make_unique<EventHandle>(std::move(exitEvent)));

            if (input.has_value())
            {
                io.AddHandle(std::make_unique<WriteHandle>(std::move(stdinHandle), std::vector<char>(input->begin(), input->end())));
            }
            else
            {
                stdinHandle.reset();
            }

            io.Run(60000ms);

            int status = 0;
            THROW_IF_FAILED(g_api->WSLCProcessGetExitCode(process, &status));
            g_logfile << "Command: '" << cmd << "', status=" << status << ", stdout: " << out << ", stderr: " << err << std::endl;

            return {status, out, err};
        };

        // Test process creation (output & exit code validated by the test code).
        {
            runCommand("echo -n stdout-ok && echo -n stderr-ok >&2");
            runCommand("cat", "stdin-ok");
            runCommand("exit 12");
            runCommand("echo -n $ENV", {}, {"ENV=env-ok", nullptr});
        }

        // Validate that trying to execute a non-existent file fails with the expected error code.
        {
            WSLCProcessHandle process = nullptr;
            int errnoValue = 0;
            std::vector<const char*> args = {"does-not-exist", nullptr};

            auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, &errnoValue);
            g_logfile << "WSLCCreateProcess(does-not-exist): " << std::hex << hr << ", errno=" << std::dec << errnoValue << std::endl;
        }

        // Validate various error paths
        {
            std::vector<const char*> args = {"/bin/sh", "-c", "sleep 9999", nullptr};
            WSLCProcessHandle process = nullptr;
            THROW_IF_FAILED(g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr));
            auto releaseProcess = wil::scope_exit([&]() { g_api->WSLCReleaseProcess(process); });

            // Validate that getting an fd that doesn't exist fails with the expected error code.
            HANDLE dummy = nullptr;
            g_logfile << "WSLCProcessGetFd(999): " << g_api->WSLCProcessGetFd(process, static_cast<WSLCProcessFd>(999), &dummy) << std::endl;
            int exitCode = -1;

            g_logfile << "WSLCProcessGetExitCode(<running>): " << g_api->WSLCProcessGetExitCode(process, &exitCode) << std::endl;
        }

        const auto testFolder = L"C:\\";
        constexpr auto testFileName = L"plugin-test.txt";

        // Validate rw mounts.
        {
            auto rwCleanup = wil::scope_exit_log(
                WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(std::wstring(testFolder) + testFileName); });

            {
                std::ofstream file(std::wstring(testFolder) + testFileName);
                file << "Windows-content";
            }

            // Mount read-write and verify the file can be read from Linux.
            char rwMountpoint[WSLC_MOUNTPOINT_LENGTH] = {};
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, testFolder, false, L"plugin-rw-test", rwMountpoint));

            g_logfile << "WSLC RW folder mounted at: " << rwMountpoint << std::endl;

            auto readCmd = std::format("cat {}/{}", rwMountpoint, testFileName);
            runCommand(readCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, rwMountpoint));
        }

        // Validate ro mounts.
        {
            char roMountpoint[WSLC_MOUNTPOINT_LENGTH] = {};
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, L"C:\\", TRUE, L"plugin-ro-test", roMountpoint));

            g_logfile << "WSLC RO folder mounted at: " << roMountpoint << std::endl;

            // Attempt to write from Linux — should fail on a read-only mount.
            auto writeCmd = std::format("echo fail > {}/should-not-exist.txt", roMountpoint);
            runCommand(writeCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, roMountpoint));
        }

        // Validate that trying to mount a folder that doesn't exist fails with the expected error code.
        {
            char mountpoint[WSLC_MOUNTPOINT_LENGTH] = {};
            g_logfile << "WSLCMountFolder(nonexistent): "
                      << g_api->WSLCMountFolder(Session->SessionId, L"C:\\nonexistent", TRUE, L"plugin-ro-test", mountpoint) << std::endl;
        }

        // Validate that trying to escape the /mnt folder fails.
        {
            char mountpoint[WSLC_MOUNTPOINT_LENGTH] = {};
            g_logfile << "WSLCMountFolder(../escape): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\", TRUE, L"../escape", mountpoint)
                      << std::endl;
        }

        // Validate that empty names are rejected.
        {
            char mountpoint[WSLC_MOUNTPOINT_LENGTH] = {};
            g_logfile << "WSLCMountFolder(): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\", TRUE, L"", mountpoint) << std::endl;
        }

        g_logfile << "Test completed" << std::endl;
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT OnWslcSessionStopping(const WSLCSessionInformation* Session)
{
    g_logfile << "WSLC Session stopping, name=" << wsl::shared::string::WideToMultiByte(Session->DisplayName)
              << ", id=" << Session->SessionId << std::endl;

    return S_OK;
}

HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    auto container = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(InspectJson);

    g_logfile << "WSLC Container started, session=" << Session->SessionId << ", id=" << container.Id
              << ", name=" << container.Name << ", image=" << container.Image << ", state=" << container.State.Status << std::endl;

    if (g_testType == PluginTestType::WslcContainerRejected)
    {
        g_logfile << "OnWslcContainerStarted: ERROR_ACCESS_DENIED" << std::endl;
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId)
{
    g_logfile << "WSLC Container stopping, session=" << Session->SessionId << ", id=" << ContainerId << std::endl;
    return S_OK;
}

HRESULT OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson)
{
    auto image = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectImage>(InspectJson);
    auto name = (image.RepoTags.has_value() && !image.RepoTags->empty()) ? image.RepoTags->front() : "<none>";
    g_logfile << "WSLC Image created, session=" << Session->SessionId << ", id=" << image.Id << ", name=" << name << std::endl;
    return S_OK;
}

HRESULT OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId)
{
    g_logfile << "WSLC Image deleted, session=" << Session->SessionId << ", id=" << ImageId << std::endl;
    return S_OK;
}

EXTERN_C __declspec(dllexport) HRESULT WSLPLUGINAPI_ENTRYPOINTV1(const WSLPluginAPIV1* Api, WSLPluginHooksV1* Hooks)
{
    try
    {
        const auto key = OpenTestRegistryKey(KEY_READ);

        const std::wstring outputFile = ReadString(key.get(), nullptr, c_logFile);
        g_logfile.open(outputFile);
        THROW_HR_IF(E_UNEXPECTED, !g_logfile);

        g_testType = static_cast<PluginTestType>(ReadDword(key.get(), nullptr, c_testType, static_cast<DWORD>(PluginTestType::Invalid)));
        THROW_HR_IF(E_INVALIDARG, static_cast<DWORD>(g_testType) <= 0 || static_cast<DWORD>(g_testType) > static_cast<DWORD>(PluginTestType::WslcImagePull));

        g_logfile << "Plugin loaded. TestMode=" << static_cast<DWORD>(g_testType) << std::endl;
        g_api = Api;
        Hooks->OnVMStarted = &OnVmStarted;
        Hooks->OnVMStopping = &OnVmStopping;
        Hooks->OnDistributionStarted = &OnDistroStarted;
        Hooks->OnDistributionStopping = &OnDistroStopping;
        Hooks->OnDistributionRegistered = &OnDistributionRegistered;
        Hooks->OnDistributionUnregistered = &OnDistributionUnregistered;
        Hooks->OnSessionCreated = &OnWslcSessionCreated;
        Hooks->OnSessionStopping = &OnWslcSessionStopping;
        Hooks->ContainerStarted = &OnWslcContainerStarted;
        Hooks->ContainerStopping = &OnWslcContainerStopping;
        Hooks->ImageCreated = &OnWslcImageCreated;
        Hooks->ImageDeleted = &OnWslcImageDeleted;

        if (g_testType == PluginTestType::FailToLoad)
        {
            g_logfile << "OnLoad: E_UNEXPECTED" << std::endl;
            return E_UNEXPECTED;
        }
        else if (g_testType == PluginTestType::PluginRequiresUpdate)
        {
            g_logfile << "OnLoad: WSL_E_PLUGINREQUIRESUPDATE" << std::endl;

            WSL_PLUGIN_REQUIRE_VERSION(9999, 99, 99, Api);
        }
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        if (g_logfile)
        {
            g_logfile << "Failed to initialize plugin, " << error << std::endl;
        }

        return error;
    }
    return S_OK;
}
