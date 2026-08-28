/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plugin.cpp

Abstract:

    This file contains a test plugin.

--*/

#include "precomp.h"
#include <atomic>
#include <thread>
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

// Process deliberately left running across OnWslcVmStopping by the WslcVmStopCommitted test, to
// prove the announced teardown happens anyway. Never released: it dies with the VM.
//
// The exit event is fetched on the callback's own thread and cached here as a plain Win32 handle:
// the process itself is a COM proxy marshalled to that thread, so the stop-window thread below
// cannot call methods on it (RPC_E_WRONG_THREAD), but it can wait on the handle.
std::atomic<WSLCProcessHandle> g_leakedProcess = nullptr;
std::atomic<HANDLE> g_leakedProcessExitEvent = nullptr;

// Set by the WslcVmStopCommitted test: a call issued from a thread the plugin owns while
// OnWslcVmStopping is running. Like the callback itself it is served by the VM that is stopping, and
// must not block on the teardown. It deliberately logs nothing of its own -- its results are written
// when it is joined -- so g_logfile keeps a single writer and the expected output stays ordered.
std::thread g_stopWindowCaller;
HRESULT g_stopWindowCallerResult = E_PENDING;
std::atomic<bool> g_leakedProcessDied = false;

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
    else if (g_testType == PluginTestType::MountFolderAccess)
    {
        const auto key = OpenTestRegistryKey(KEY_READ);
        const auto mountSource = ReadString(key.get(), nullptr, c_mountFolder);

        RETURN_IF_FAILED(
            g_api->MountFolder(Session->SessionId, mountSource.c_str(), L"/test-plugin-access", false, L"test-plugin-access"));

        std::vector<const char*> arguments = {"/bin/sh", "-c", "{ echo test > /test-plugin-access/plugin-test.txt; } 2>&1", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket));

        const auto output = ReadFromSocket(socket.get());
        g_logfile.write(output.data(), output.size());
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

    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        // A plugin call is never a reason to create a VM. This one has to be rejected rather than
        // bringing one up, which the absence of any VM notification in the expected output confirms.
        std::vector<const char*> args = {"/bin/true", nullptr};
        WSLCProcessHandle process = nullptr;
        const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
        if (SUCCEEDED(hr))
        {
            g_api->WSLCReleaseProcess(process);
        }

        g_logfile << "WSLC no-vm caller: " << (hr == WSLC_E_VM_NOT_RUNNING ? "rejected" : "unexpected") << std::endl;
        return S_OK;
    }

    if (g_testType == PluginTestType::WslcSessionRejected)
    {
        g_logfile << "OnWslcSessionCreated: ERROR_ACCESS_DENIED" << std::endl;
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    return S_OK;
}
CATCH_RETURN();

// These checks need a running VM, and a plugin call never creates one, so they run from the VM-started
// hook rather than from session creation.
void RunWslcSuccessChecks(const WSLCSessionInformation* Session)
{
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
        constexpr auto rwMountpoint = "/mnt/wsl-plugin/plugin-rw-test";
        constexpr auto roMountpoint = "/mnt/wsl-plugin/plugin-ro-test";

        // Validate rw mounts.
        {
            auto rwCleanup = wil::scope_exit_log(
                WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(std::wstring(testFolder) + testFileName); });

            {
                std::ofstream file(std::wstring(testFolder) + testFileName);
                file << "Windows-content";
            }

            // Mount read-write and verify the file can be read from Linux.
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, testFolder, rwMountpoint, false));

            g_logfile << "WSLC RW folder mounted at: " << rwMountpoint << std::endl;

            auto readCmd = std::format("cat {}/{}", rwMountpoint, testFileName);
            runCommand(readCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, rwMountpoint));
        }

        // Validate ro mounts.
        {
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, L"C:\\", roMountpoint, TRUE));

            g_logfile << "WSLC RO folder mounted at: " << roMountpoint << std::endl;

            // Attempt to write from Linux — should fail on a read-only mount.
            auto writeCmd = std::format("echo fail > {}/should-not-exist.txt", roMountpoint);
            runCommand(writeCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, roMountpoint));
        }

        // Validate that trying to mount a folder that doesn't exist fails with the expected error code.
        g_logfile << "WSLCMountFolder(nonexistent): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\nonexistent", roMountpoint, TRUE)
                  << std::endl;

        // Validate that non-absolute mountpoints are rejected.
        g_logfile << "WSLCMountFolder(relative): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\", "relative-mountpoint", TRUE)
                  << std::endl;

        g_logfile << "Test completed" << std::endl;
    }
}

HRESULT OnWslcSessionStopping(const WSLCSessionInformation* Session)
{
    // Drain the stop-window thread first, then report what it observed. Logging from here rather than
    // from that thread keeps the log single-writer and the expected output deterministic. Safe
    // because this is the last event of the session, so the join cannot run inside a VM notification.
    if (g_stopWindowCaller.joinable())
    {
        g_stopWindowCaller.join();

        g_logfile << "WSLC stop-window caller: " << (SUCCEEDED(g_stopWindowCallerResult) ? "ok" : "failed") << std::endl;
        g_logfile << "WSLC leaked process died: " << (g_leakedProcessDied.load() ? "yes" : "no") << std::endl;
    }

    // Close the duplicated exit event if the stop-window thread did not get far enough to claim it.
    // The leaked process wrapper is deliberately not released: it holds a COM proxy marshalled to the
    // OnWslcVmStopping callback's thread, and releasing it from this one risks the same
    // RPC_E_WRONG_THREAD hazard that forced the exit event to be cached as a plain handle. It is one
    // wrapper for the lifetime of a test process, so leaking it is the safer trade.
    if (auto* exitEvent = g_leakedProcessExitEvent.exchange(nullptr); exitEvent != nullptr)
    {
        const wil::unique_handle owned{exitEvent};
    }

    g_logfile << "WSLC Session stopping, name=" << wsl::shared::string::WideToMultiByte(Session->DisplayName)
              << ", id=" << Session->SessionId << std::endl;

    return S_OK;
}

HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    auto container = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(InspectJson);

    g_logfile << "WSLC Container started, session=" << Session->SessionId << ", id=" << container.Id << ", name=" << container.Name
              << ", image=" << container.Config.Image << ", state=" << container.State.Status << std::endl;

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

HRESULT OnWslcVmStarted(const WSLCSessionInformation* Session)
try
{
    if (g_testType == PluginTestType::WslcSuccess)
    {
        // Run once: the checks are written against the first VM of the session.
        static std::atomic<bool> done = false;
        if (!done.exchange(true))
        {
            RunWslcSuccessChecks(Session);
        }

        return S_OK;
    }

    if (g_testType == PluginTestType::WslcVmStopCommitted)
    {
        g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    // The VM-never-started test expects no VM hook to fire at all. Logging here is the diagnostic
    // that makes a regression visible: any line from this hook fails the expected output.
    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    // Only log/exercise for the dedicated VM-restart test so other WSLC plugin tests (which start
    // and stop VMs incidentally) are not affected by extra log lines.
    if (g_testType != PluginTestType::WslcVmRestart)
    {
        return S_OK;
    }

    g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;

    // Prove the VM is usable from within the started hook, and that calling back into the session
    // (WSLCCreateProcess acquires a VM lease + the runtime lock) does not deadlock.
    std::vector<const char*> args = {"/bin/true", nullptr};
    WSLCProcessHandle process = nullptr;
    const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
    g_logfile << "WSLC VM started reentrant WSLCCreateProcess: " << (SUCCEEDED(hr) ? "ok" : "failed") << std::endl;
    if (SUCCEEDED(hr))
    {
        g_api->WSLCReleaseProcess(process);
    }

    // Also exercise a reentrant mount + unmount from the started hook; the session is alive here so
    // both calls succeed, validating that mount management reentrant from OnVmStarted does not deadlock.
    constexpr auto* mountpoint = "/test-plugin/vm-started-mount";
    const auto mountHr = g_api->WSLCMountFolder(Session->SessionId, L"C:\\", mountpoint, TRUE);
    if (SUCCEEDED(mountHr))
    {
        const auto unmountHr = g_api->WSLCUnmountFolder(Session->SessionId, mountpoint);
        g_logfile << "WSLC VM started mount+unmount: " << (SUCCEEDED(unmountHr) ? "ok" : "failed") << std::endl;
    }
    else
    {
        g_logfile << "WSLC VM started mount+unmount: skipped" << std::endl;
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT OnWslcVmStopping(const WSLCSessionInformation* Session)
try
{
    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    if (g_testType == PluginTestType::WslcVmStopCommitted)
    {
        // Only the idle teardown is interesting here. The session's final teardown races with the
        // session-stopping notification, which is delivered independently, so logging it would make
        // the expected output order-dependent on that race.
        if (g_stopWindowCaller.joinable())
        {
            return S_OK;
        }

        g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;

        // Deliberately leave a live process behind when this callback returns. The stop is committed
        // before it is announced, so the VM goes away regardless and this process dies with it --
        // which is exactly what the callback was just told would happen.
        //
        // Created before the thread below is started so its exit event is published first: that thread
        // claims the event and must not race ahead of it.
        std::vector<const char*> args = {"/bin/sleep", "60", nullptr};
        WSLCProcessHandle leaked = nullptr;
        const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &leaked, nullptr);
        g_leakedProcess.store(leaked);

        // Cache the exit event while still on the thread the process proxy is marshalled to.
        if (SUCCEEDED(hr))
        {
            HANDLE exitEvent = nullptr;
            if (SUCCEEDED(g_api->WSLCProcessGetExitEvent(leaked, &exitEvent)))
            {
                g_leakedProcessExitEvent.store(exitEvent);
            }
        }

        g_logfile << "WSLC VM stopping leaked process: " << (SUCCEEDED(hr) ? "ok" : "failed") << std::endl;

        // A call from a thread this plugin owns is served on the same terms as the callback itself:
        // by the VM that is stopping, not by a future one and not by a new one. It must not block on
        // the teardown. Results are logged when this thread is joined, so the output stays deterministic.
        const auto sessionId = Session->SessionId;
        g_stopWindowCaller = std::thread([sessionId]() {
            std::vector<const char*> processArgs = {"/bin/true", nullptr};
            WSLCProcessHandle process = nullptr;
            g_stopWindowCallerResult = g_api->WSLCCreateProcess(sessionId, processArgs[0], processArgs.data(), nullptr, &process, nullptr);
            if (SUCCEEDED(g_stopWindowCallerResult))
            {
                g_api->WSLCReleaseProcess(process);
            }

            // The announced stop takes the VM away and the leaked process with it. Prove that
            // directly instead of inferring it from the fact that a new VM started -- a process that
            // outlived the stop is the exact symptom of an announced stop that did not happen.
            if (auto* exitEvent = g_leakedProcessExitEvent.exchange(nullptr); exitEvent != nullptr)
            {
                // GetExitEvent is marshalled as an [out, system_handle(sh_event)] parameter, so this
                // is a duplicate owned by this process.
                const wil::unique_handle owned{exitEvent};
                g_leakedProcessDied = WaitForSingleObject(owned.get(), 30 * 1000) == WAIT_OBJECT_0;
            }
        });

        // Give the thread time to issue its call inside the stop window. If it has not, the test still
        // passes -- it just proves less.
        std::this_thread::sleep_for(500ms);

        return S_OK;
    }

    if (g_testType != PluginTestType::WslcVmRestart)
    {
        return S_OK;
    }

    g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;

    // Proves OnVmStopping doesn't deadlock a plugin that calls back in: on idle teardown these are
    // served by the VM that is still stopping and succeed; on permanent teardown they fail cleanly.
    std::vector<const char*> args = {"/bin/true", nullptr};
    WSLCProcessHandle process = nullptr;
    const auto processHr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
    g_logfile << "WSLC VM stopping reentrant WSLCCreateProcess: " << (SUCCEEDED(processHr) ? "ok" : "failed") << std::endl;
    if (SUCCEEDED(processHr))
    {
        g_api->WSLCReleaseProcess(process);
    }

    constexpr auto* mountpoint = "/test-plugin/vm-stopping-mount";
    const auto mountHr = g_api->WSLCMountFolder(Session->SessionId, L"C:\\", mountpoint, TRUE);
    if (SUCCEEDED(mountHr))
    {
        const auto unmountHr = g_api->WSLCUnmountFolder(Session->SessionId, mountpoint);
        g_logfile << "WSLC VM stopping mount+unmount: " << (SUCCEEDED(unmountHr) ? "ok" : "failed") << std::endl;
    }
    else
    {
        g_logfile << "WSLC VM stopping mount+unmount: skipped" << std::endl;
    }

    return S_OK;
}
CATCH_RETURN();

EXTERN_C __declspec(dllexport) HRESULT WSLPLUGINAPI_ENTRYPOINTV1(const WSLPluginAPIV1* Api, WSLPluginHooksV1* Hooks)
{
    try
    {
        const auto key = OpenTestRegistryKey(KEY_READ);

        const std::wstring outputFile = ReadString(key.get(), nullptr, c_logFile);
        g_logfile.open(outputFile);
        THROW_HR_IF(E_UNEXPECTED, !g_logfile);

        g_testType = static_cast<PluginTestType>(ReadDword(key.get(), nullptr, c_testType, static_cast<DWORD>(PluginTestType::Invalid)));
        THROW_HR_IF(E_INVALIDARG, static_cast<DWORD>(g_testType) <= 0 || static_cast<DWORD>(g_testType) > static_cast<DWORD>(PluginTestType::MountFolderAccess));

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
        Hooks->WslcVmStarted = &OnWslcVmStarted;
        Hooks->WslcVmStopping = &OnWslcVmStopping;

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
