/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plugin.cpp

Abstract:

    This file contains a test plugin.

--*/

#include "precomp.h"
#include "WslPluginApi.h"

#include "PluginTests.h"

using namespace wsl::windows::common::registry;

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

// Extracts the value of a "key":"value" string field from a flat JSON document.
// Returns an empty string if the field is not present. Used by WSLC handlers to
// keep logs deterministic without pulling in a full JSON parser.
static std::string ExtractJsonStringField(const char* json, std::string_view key)
{
    if (json == nullptr)
    {
        return {};
    }

    std::string needle = "\"";
    needle.append(key);
    needle.append("\":\"");

    const char* start = strstr(json, needle.c_str());
    if (start == nullptr)
    {
        return {};
    }
    start += needle.size();

    std::string out;
    while (*start != '\0' && *start != '"')
    {
        if (*start == '\\' && start[1] != '\0')
        {
            out.push_back(start[1]);
            start += 2;
            continue;
        }
        out.push_back(*start);
        ++start;
    }
    return out;
}

HRESULT OnWslcSessionCreated(const WSLCSessionInformation* Session)
{
    g_logfile << "WSLC Session created, name=" << wsl::shared::string::WideToMultiByte(Session->DisplayName)
              << ", id=" << Session->SessionId << ", pid=" << Session->ApplicationPid
              << ", token=" << (Session->UserToken != nullptr ? "set" : "null")
              << ", sid=" << (Session->UserSid != nullptr ? "set" : "null") << std::endl;

    if (g_testType == PluginTestType::WslcSessionRejected)
    {
        g_logfile << "OnWslcSessionCreated: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnWslcSessionStopping(const WSLCSessionInformation* Session)
{
    g_logfile << "WSLC Session stopping, name=" << wsl::shared::string::WideToMultiByte(Session->DisplayName)
              << ", id=" << Session->SessionId << std::endl;

    return S_OK;
}

HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectContainer)
{
    auto id = ExtractJsonStringField(InspectContainer, "Id");
    auto image = ExtractJsonStringField(InspectContainer, "Image");

    g_logfile << "WSLC Container started, session=" << Session->SessionId << ", id=" << id << ", image=" << image << std::endl;

    if (g_testType == PluginTestType::WslcContainerRejected)
    {
        g_logfile << "OnWslcContainerStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    if (g_testType == PluginTestType::WslcSuccess && g_api != nullptr && g_api->Wslc != nullptr)
    {
        // Mount a Windows folder, run a process in the root namespace, wait for it to exit,
        // then unmount. Validates the full plugin -> WSLC API surface in one go.
        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        auto mountSource = modulePath.parent_path().wstring();

        wchar_t mountpoint[256] = {};
        const auto mountResult =
            g_api->Wslc->MountFolder(Session->SessionId, mountSource.c_str(), TRUE, L"plugin-test-mount", mountpoint);
        if (FAILED(mountResult))
        {
            g_logfile << "WSLC MountFolder failed: " << mountResult << std::endl;
            return S_OK;
        }

        g_logfile << "WSLC Folder mounted at: " << wsl::shared::string::WideToMultiByte(mountpoint) << std::endl;

        std::vector<const char*> arguments = {"/bin/sh", "-c", "echo wslc-plugin-ok", nullptr};
        WSLCProcess process{};
        const auto procResult = g_api->Wslc->CreateProcess(Session->SessionId, arguments[0], arguments.data(), nullptr, &process);
        if (FAILED(procResult))
        {
            g_logfile << "WSLC CreateProcess failed: " << procResult << std::endl;
        }
        else
        {
            g_logfile << "WSLC Process created, pid=" << process.Pid << std::endl;

            int status = 0;
            const auto waitResult = g_api->Wslc->WaitPid(Session->SessionId, static_cast<LONG>(process.Pid), 30000, &status);
            g_logfile << "WSLC WaitPid result=" << waitResult << ", status=" << status << std::endl;

            if (process.Stdin != INVALID_SOCKET)
            {
                closesocket(process.Stdin);
            }
            if (process.Stdout != INVALID_SOCKET)
            {
                closesocket(process.Stdout);
            }
            if (process.Stderr != INVALID_SOCKET)
            {
                closesocket(process.Stderr);
            }
            if (process.ExitEvent != nullptr)
            {
                CloseHandle(process.ExitEvent);
            }
        }

        const auto unmountResult = g_api->Wslc->UnmountFolder(Session->SessionId, mountpoint);
        g_logfile << "WSLC Folder unmounted, result=" << unmountResult << std::endl;
    }

    return S_OK;
}

HRESULT OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR InspectContainer)
{
    auto id = ExtractJsonStringField(InspectContainer, "Id");
    g_logfile << "WSLC Container stopping, session=" << Session->SessionId << ", id=" << id << std::endl;
    return S_OK;
}

HRESULT OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectImage)
{
    auto id = ExtractJsonStringField(InspectImage, "Id");
    g_logfile << "WSLC Image created, session=" << Session->SessionId << ", id=" << id << std::endl;
    return S_OK;
}

HRESULT OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR InspectImage)
{
    auto id = ExtractJsonStringField(InspectImage, "Id");
    g_logfile << "WSLC Image deleted, session=" << Session->SessionId << ", id=" << id << std::endl;
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
        THROW_HR_IF(E_INVALIDARG, static_cast<DWORD>(g_testType) <= 0 || static_cast<DWORD>(g_testType) > static_cast<DWORD>(PluginTestType::WslcContainerRejected));

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