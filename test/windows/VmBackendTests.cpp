// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "hcs.hpp"
#include "OpenVmmError.h"
#include <shellapi.h>
#include <TlHelp32.h>
#include <array>

using namespace wsl::windows::common;
using namespace wsl::shared::string;

class VmBackendTests
{
    WSL_TEST_CLASS(VmBackendTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_IS_TRUE(LxsstuInitialize(FALSE));
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }

    static std::wstring BackendConfig(bool OpenVmm)
    {
        return LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false}) +
               std::format(L"\n[experimental]\nopenVmm={}\n", OpenVmm ? L"true" : L"false");
    }

    static std::wstring ReadVmId()
    {
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"--exec wslinfo --vm-id -n", 0);
        VERIFY_ARE_EQUAL(warnings, L"");
        const auto id = ToGuid(output);
        VERIFY_IS_TRUE(id.has_value());
        VERIFY_IS_FALSE(IsEqualGUID(id.value(), GUID_NULL));
        return GuidToString<wchar_t>(id.value(), GuidToStringFlags::None);
    }

    static std::filesystem::path RpcDirectory(const std::wstring& VmId)
    {
        const auto token = security::GetUserToken(TokenImpersonation);
        return filesystem::GetTempFolderPath(token.get()) / std::format(L"wsl-{}.rpc", VmId);
    }

    static void VerifyBackend(const std::wstring& VmId, bool OpenVmm)
    {
        hcs::unique_hcs_system system;
        const auto result = HcsOpenComputeSystem(VmId.c_str(), GENERIC_READ, system.put());
        VERIFY_ARE_EQUAL(result, OpenVmm ? HCS_E_SYSTEM_NOT_FOUND : S_OK);
        // Correlate the OpenVMM RPC endpoint with the guest's VM ID, not an unrelated host process.
        VERIFY_ARE_EQUAL(std::filesystem::exists(RpcDirectory(VmId) / L"s"), OpenVmm);
    }

    static void VerifyStopped(const std::wstring& VmId)
    {
        hcs::unique_hcs_system system;
        VERIFY_ARE_EQUAL(HcsOpenComputeSystem(VmId.c_str(), GENERIC_READ, system.put()), HCS_E_SYSTEM_NOT_FOUND);
        VERIFY_IS_FALSE(std::filesystem::exists(RpcDirectory(VmId)));
    }

#if WSL_INCLUDE_OPENVMM
    static constexpr DWORD c_operationTimeout = 120000;

    struct Command
    {
        wil::unique_handle process;
        wil::unique_handle output;
        wil::unique_handle input;

        explicit Command(LPCWSTR Arguments)
        {
            auto [readOutput, writeOutput] = CreateSubprocessPipe(false, true, 65536);
            auto [readInput, writeInput] = CreateSubprocessPipe(true, false);
            auto commandLine = LxssGenerateWslCommandLine(Arguments);
            process = LxsstuStartProcess(commandLine.data(), readInput.get(), writeOutput.get(), writeOutput.get());
            output = std::move(readOutput);
            input = std::move(writeInput);
        }

        ~Command()
        {
            if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
            {
                // Only terminate the retained test client, never a process rediscovered by PID.
                LOG_IF_WIN32_BOOL_FALSE(TerminateProcess(process.get(), ERROR_CANCELLED));
                WaitForSingleObject(process.get(), 5000);
            }
        }

        DWORD Wait() const
        {
            const auto wait = WaitForSingleObject(process.get(), c_operationTimeout);
            if (wait != WAIT_OBJECT_0)
            {
                LogError("WSL client %lu did not complete: wait=%lu", GetProcessId(process.get()), wait);
            }
            VERIFY_ARE_EQUAL(wait, WAIT_OBJECT_0);
            DWORD exitCode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetExitCodeProcess(process.get(), &exitCode));
            return exitCode;
        }

        std::string ReadAvailable() const
        {
            DWORD available{};
            if (!PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr))
            {
                VERIFY_ARE_EQUAL(GetLastError(), static_cast<DWORD>(ERROR_BROKEN_PIPE));
                return {};
            }
            std::string result(available, '\0');
            if (available != 0)
            {
                DWORD read{};
                VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(output.get(), result.data(), available, &read, nullptr));
                result.resize(read);
            }
            return result;
        }

        void WaitForReady() const
        {
            const auto deadline = GetTickCount64() + c_operationTimeout;
            while (ReadAvailable().find('R') == std::string::npos)
            {
                VERIFY_ARE_EQUAL(WaitForSingleObject(process.get(), 20), WAIT_TIMEOUT);
                VERIFY_IS_TRUE(GetTickCount64() < deadline);
            }
            VERIFY_ARE_EQUAL(WaitForSingleObject(process.get(), 0), WAIT_TIMEOUT);
        }
    };

    static std::wstring ReadVmIdBounded()
    {
        Command command(L"--exec wslinfo --vm-id -n");
        VERIFY_ARE_EQUAL(command.Wait(), 0u);
        const auto id = ToGuid(MultiByteToWide(command.ReadAvailable()));
        VERIFY_IS_TRUE(id.has_value());
        VERIFY_IS_FALSE(IsEqualGUID(id.value(), GUID_NULL));
        return GuidToString<wchar_t>(id.value(), GuidToStringFlags::None);
    }

    static void ShutdownBounded(bool Force = false)
    {
        Command command(Force ? L"--shutdown --force" : L"--shutdown");
        VERIFY_ARE_EQUAL(command.Wait(), 0u);
    }

    static wil::unique_handle ServiceProcess()
    {
        const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        VERIFY_IS_TRUE(!!manager);
        const wil::unique_schandle service{OpenServiceW(manager.get(), L"WslService", SERVICE_QUERY_STATUS)};
        VERIFY_IS_TRUE(!!service);
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes{};
        VERIFY_WIN32_BOOL_SUCCEEDED(
            QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes));
        VERIFY_ARE_EQUAL(status.dwCurrentState, static_cast<DWORD>(SERVICE_RUNNING));
        wil::unique_handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, status.dwProcessId)};
        VERIFY_IS_TRUE(!!process);
        return process;
    }

    static wil::unique_handle FindVmProcess(const std::wstring& VmId)
    {
        // Query the command line through the same handle later used for termination/waiting.
        // ProcessCommandLineInformation (60) avoids remote PEB reads and cross-bitness assumptions.
        using QueryProcess = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        const auto query =
            reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        VERIFY_IS_NOT_NULL(query);
        const wil::unique_handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
        VERIFY_IS_TRUE(!!snapshot);
        PROCESSENTRY32W entry{sizeof(entry)};
        VERIFY_WIN32_BOOL_SUCCEEDED(Process32FirstW(snapshot.get(), &entry));
        wil::unique_handle result;
        const auto expectedArgument = std::format(L"path={},transport=grpc", (RpcDirectory(VmId) / L"s").wstring());
        do
        {
            if (_wcsicmp(entry.szExeFile, L"openvmm.exe") != 0)
            {
                continue;
            }
            wil::unique_handle process{
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID)};
            if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
            {
                continue;
            }
            ULONG size{};
            query(process.get(), 60, nullptr, 0, &size);
            if (size == 0)
            {
                continue;
            }
            std::vector<BYTE> buffer(size);
            if (query(process.get(), 60, buffer.data(), size, &size) < 0)
            {
                continue;
            }
            struct CommandLine
            {
                USHORT length;
                USHORT maximumLength;
                PWSTR buffer;
            };
            const auto& line = *reinterpret_cast<const CommandLine*>(buffer.data());
            const std::wstring commandLine(line.buffer, line.length / sizeof(wchar_t));
            int count{};
            const wil::unique_hlocal_ptr<LPWSTR[]> arguments{CommandLineToArgvW(commandLine.c_str(), &count)};
            VERIFY_IS_TRUE(!!arguments);
            if (count == 3 && std::wstring_view(arguments[1]) == L"--rpc" && arguments[2] == expectedArgument)
            {
                VERIFY_IS_FALSE(!!result);
                result = std::move(process);
            }
        } while (Process32NextW(snapshot.get(), &entry));
        return result;
    }

    static bool EndpointsExist(const std::wstring& VmId)
    {
        if (std::filesystem::exists(RpcDirectory(VmId)))
        {
            return true;
        }
        const auto prefix = std::format(L"wsl-{}.v", VmId.substr(0, 8));
        for (const auto& entry : std::filesystem::directory_iterator(RpcDirectory(VmId).parent_path()))
        {
            const auto name = entry.path().filename().wstring();
            // OpenVMM deliberately preserves .v.log diagnostics; only transport endpoints must disappear.
            if (name == prefix || name.starts_with(prefix + L"_"))
            {
                return true;
            }
        }
        return false;
    }

    static void VerifyOpenVmmStopped(const std::wstring& VmId, HANDLE Process = nullptr)
    {
        if (Process)
        {
            VERIFY_ARE_EQUAL(WaitForSingleObject(Process, c_operationTimeout), WAIT_OBJECT_0);
        }
        const auto deadline = GetTickCount64() + c_operationTimeout;
        while (EndpointsExist(VmId))
        {
            VERIFY_IS_TRUE(GetTickCount64() < deadline);
            Sleep(20);
        }
        VerifyStopped(VmId);
        VERIFY_IS_FALSE(!!FindVmProcess(VmId));
    }

    static void VerifySameService(HANDLE Process)
    {
        VERIFY_ARE_EQUAL(WaitForSingleObject(Process, 0), WAIT_TIMEOUT);
        const auto current = ServiceProcess();
        VERIFY_ARE_EQUAL(GetProcessId(Process), GetProcessId(current.get()));
    }

    static void VerifyFreshOpenVmm(const std::wstring& OldId, HANDLE Service)
    {
        const auto id = ReadVmIdBounded();
        VERIFY_ARE_NOT_EQUAL(id, OldId);
        VerifyBackend(id, true);
        const auto process = FindVmProcess(id);
        VERIFY_IS_TRUE(!!process);
        Command command(L"--exec true");
        VERIFY_ARE_EQUAL(command.Wait(), 0u);
        VerifySameService(Service);
        ShutdownBounded();
        VerifyOpenVmmStopped(id, process.get());
        VerifySameService(Service);
    }
#endif

    TEST_METHOD(OpenVmmFailureClassification)
    {
        struct TestCase
        {
            HRESULT result;
            PCSTR category;
            bool requiresRecreation;
        };
        const TestCase cases[] = {
            {E_INVALIDARG, "Validation", false},
            {HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "Validation", false},
            {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "Validation", false},
            {E_NOTIMPL, "Validation", false},
            {E_ACCESSDENIED, "Authorization", false},
            {HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE), "Authorization", false},
            {HRESULT_FROM_WIN32(WAIT_TIMEOUT), "Timeout", true},
            {E_ABORT, "Cancellation", true},
            {HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), "Transport", true},
            {HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Protocol", true},
            {HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "InvalidState", true},
            {E_FAIL, "ServerOrResource", true},
            {E_OUTOFMEMORY, "ServerOrResource", true},
            {HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE), "ServerOrResource", true},
            {S_OK, "ServerOrResource", false},
            {S_FALSE, "ServerOrResource", false}};
        for (const auto& test : cases)
        {
            LogInfo("Classifying OpenVMM HRESULT 0x%08lx", static_cast<ULONG>(test.result));
            VERIFY_ARE_EQUAL(std::string(test.category), std::string(openvmm::FailureCategory(test.result)));
            VERIFY_ARE_EQUAL(test.requiresRecreation, openvmm::RequiresRecreation(test.result));
        }
    }

    TEST_METHOD(SelectionConfigParsing)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto path = std::filesystem::temp_directory_path() / (GuidToString<wchar_t>(id) + L".wslconfig");
        HostFileChange file(path, "");
        for (const auto& [contents, expected] :
             {std::pair{"", false},
              {"[experimental]\nopenVmm=false\n", false},
              {"[experimental]\nopenVmm=true\n", true},
              {"[experimental]\nopenVmm=NotABoolean\n", false}})
        {
            file.Update(contents);
            const wsl::core::Config config(path.c_str());
            VERIFY_ARE_EQUAL(config.EnableOpenVmm, expected);
        }
    }

    WSL2_TEST_METHOD(HcsDefaultAndExplicitSelection)
    {
        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false}));
        VERIFY_IS_TRUE(WslShutdown());
        for (const bool force : {false, true})
        {
            const auto id = ReadVmId();
            VerifyBackend(id, false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(id);
            config.Update(BackendConfig(false));
        }
    }

    WSL2_TEST_METHOD(OpenVmmSelectionAndRollback)
    {
        WslConfigChange config(BackendConfig(true));
        VERIFY_IS_TRUE(WslShutdown());
#if WSL_INCLUDE_OPENVMM
        for (const bool force : {false, true})
        {
            LxssWriteWslConfig(BackendConfig(true));
            const auto openVmmId = ReadVmId();
            VerifyBackend(openVmmId, true);

            // Do not use config.Update(): restarting the service would hide incorrect live-VM dispatch.
            LxssWriteWslConfig(BackendConfig(false));
            VERIFY_ARE_EQUAL(ReadVmId(), openVmmId);
            VerifyBackend(openVmmId, true);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(openVmmId);

            const auto hcsId = ReadVmId();
            VERIFY_ARE_NOT_EQUAL(hcsId, openVmmId);
            VerifyBackend(hcsId, false);
            LxssWriteWslConfig(BackendConfig(true));
            VERIFY_ARE_EQUAL(ReadVmId(), hcsId);
            VerifyBackend(hcsId, false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(hcsId);
        }
#else
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            ValidateOutput(
                L"--exec true",
                FormatErrorMessage(
                    wsl::shared::Localization::MessageOpenVmmNotIncluded(), L"Wsl/Service/CreateInstance/CreateVm/E_NOTIMPL"));
        }

        LxssWriteWslConfig(BackendConfig(false));
        const auto id = ReadVmId();
        VerifyBackend(id, false);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(id);
#endif
    }

#if WSL_INCLUDE_OPENVMM
    WSL2_TEST_METHOD(OpenVmmProcessDeathAllowsFreshVm)
    {
        WslConfigChange config(BackendConfig(true));
        ShutdownBounded();
        const auto service = ServiceProcess();
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto id = ReadVmIdBounded();
            VerifyBackend(id, true);
            const auto process = FindVmProcess(id);
            VERIFY_IS_TRUE(!!process);
            Command active(L"--exec sh -c \"printf R; read value\"");
            active.WaitForReady();

            VERIFY_WIN32_BOOL_SUCCEEDED(TerminateProcess(process.get(), ERROR_PROCESS_ABORTED));
            VERIFY_ARE_NOT_EQUAL(active.Wait(), 0u);
            // Do not use shutdown or config.Update here: the service must notice real process death itself.
            VerifyOpenVmmStopped(id, process.get());
            VerifyFreshOpenVmm(id, service.get());
        }
    }

    WSL2_TEST_METHOD(OpenVmmShutdownWithActiveOperations)
    {
        WslConfigChange config(BackendConfig(true));
        ShutdownBounded();
        const auto service = ServiceProcess();
        for (const bool force : {false, true})
        {
            const auto id = ReadVmIdBounded();
            VerifyBackend(id, true);
            const auto process = FindVmProcess(id);
            VERIFY_IS_TRUE(!!process);
            Command active(L"--exec sh -c \"printf R; read value\"");
            active.WaitForReady();
            // The blocked guest session overlaps shutdown deterministically; query dispatch ordering is unconstrained.
            Command query(L"--list --running --quiet");
            Command shutdown(force ? L"--shutdown --force" : L"--shutdown");
            VERIFY_ARE_EQUAL(shutdown.Wait(), 0u);
            VERIFY_ARE_EQUAL(query.Wait(), 0u);
            VERIFY_ARE_NOT_EQUAL(active.Wait(), 0u);
            VerifyOpenVmmStopped(id, process.get());
            VerifyFreshOpenVmm(id, service.get());
        }
    }

    WSL2_TEST_METHOD(OpenVmmAllocatedCreationFailureDoesNotFallback)
    {
        GUID fileId{};
        VERIFY_SUCCEEDED(CoCreateGuid(&fileId));
        const auto imagePath = std::filesystem::current_path() / (GuidToString<wchar_t>(fileId) + L".vhd");
        HostFileChange image(imagePath, std::string(4096, '\0'));
        WslConfigChange config(BackendConfig(true));
        ShutdownBounded();
        const auto service = ServiceProcess();
        LxssWriteWslConfig(
            LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false, .systemDistro = imagePath.wstring()}) +
            L"\n[experimental]\nopenVmm=true\n");

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            // Subscribe before launch so even a short-lived failed VM's RPC listener is observed.
            const auto directory = RpcDirectory(L"").parent_path();
            wil::unique_hfile changes{CreateFileW(
                directory.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                nullptr)};
            VERIFY_IS_TRUE(!!changes);
            wil::unique_event changed{wil::EventOptions::ManualReset};
            OVERLAPPED overlapped{};
            overlapped.hEvent = changed.get();
            alignas(DWORD) std::array<BYTE, 65536> buffer{};
            const auto cancel = wil::scope_exit([&] {
                CancelIoEx(changes.get(), &overlapped);
                DWORD bytes{};
                GetOverlappedResult(changes.get(), &overlapped, &bytes, TRUE);
            });
            const auto arm = [&] {
                changed.ResetEvent();
                VERIFY_WIN32_BOOL_SUCCEEDED(ReadDirectoryChangesW(
                    changes.get(), buffer.data(), static_cast<DWORD>(buffer.size()), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr, &overlapped, nullptr));
            };
            arm();
            Command command(L"--exec true");
            std::wstring failedId;
            wil::unique_handle failedProcess;
            const auto deadline = GetTickCount64() + c_operationTimeout;
            for (;;)
            {
                if (WaitForSingleObject(changed.get(), 0) == WAIT_OBJECT_0)
                {
                    DWORD bytes{};
                    VERIFY_WIN32_BOOL_SUCCEEDED(GetOverlappedResult(changes.get(), &overlapped, &bytes, FALSE));
                    VERIFY_ARE_NOT_EQUAL(bytes, 0u);
                    DWORD offset{};
                    do
                    {
                        const auto& entry = *reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                        const std::filesystem::path path{std::wstring(entry.FileName, entry.FileNameLength / sizeof(wchar_t))};
                        const auto parent = path.parent_path().filename().wstring();
                        if (entry.Action == FILE_ACTION_ADDED && path.filename() == L"s" && parent.starts_with(L"wsl-") &&
                            parent.ends_with(L".rpc"))
                        {
                            const auto parsed = ToGuid(parent.substr(4, parent.size() - 8));
                            VERIFY_IS_TRUE(parsed.has_value());
                            const auto id = GuidToString<wchar_t>(parsed.value(), GuidToStringFlags::None);
                            VERIFY_IS_TRUE(failedId.empty() || failedId == id);
                            failedId = id;
                            if (!failedProcess)
                            {
                                failedProcess = FindVmProcess(id);
                            }
                        }
                        if (entry.NextEntryOffset == 0)
                        {
                            break;
                        }
                        offset += entry.NextEntryOffset;
                    } while (offset < bytes);
                    arm();
                }
                if (!failedId.empty() && WaitForSingleObject(command.process.get(), 0) == WAIT_OBJECT_0 &&
                    WaitForSingleObject(changed.get(), 0) == WAIT_TIMEOUT)
                {
                    break;
                }
                VERIFY_IS_TRUE(GetTickCount64() < deadline);
                Sleep(10);
            }
            VERIFY_ARE_NOT_EQUAL(command.Wait(), 0u);
            const auto output = MultiByteToWide(command.ReadAvailable());
            VERIFY_ARE_NOT_EQUAL(output.find(L"Wsl/Service/CreateInstance/CreateVm/"), std::wstring::npos);
            VERIFY_ARE_EQUAL(output.find(L"WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR"), std::wstring::npos);
            // The RPC socket is bound by openvmm.exe, proving this was not the early extension/path rejection.
            VERIFY_IS_FALSE(failedId.empty());
            VerifyOpenVmmStopped(failedId, failedProcess.get());
            VerifySameService(service.get());
            LxssWriteWslConfig(BackendConfig(true));
            VerifyFreshOpenVmm(failedId, service.get());
            LxssWriteWslConfig(
                LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false, .systemDistro = imagePath.wstring()}) +
                L"\n[experimental]\nopenVmm=true\n");
        }
    }

    WSL2_TEST_METHOD(OpenVmmCreationFailureDoesNotFallback)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto systemDistro = std::filesystem::temp_directory_path() / (GuidToString<wchar_t>(id) + L".img");
        HostFileChange image(systemDistro, std::string(4096, '\0'));
        WslConfigChange config(
            LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false, .systemDistro = systemDistro.wstring()}) +
            L"\n[experimental]\nopenVmm=true\n");
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"--exec true", -1);
            // OpenVMM rejects .img before creating resources. HCS accepts the extension and would fail later.
            VERIFY_ARE_NOT_EQUAL(output.find(L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR"), std::wstring::npos);
            VERIFY_ARE_EQUAL(warnings, L"");
        }

        // Retry and rollback within the same service session, without masking state cleanup with a service restart.
        LxssWriteWslConfig(BackendConfig(true));
        const auto openVmmId = ReadVmId();
        VerifyBackend(openVmmId, true);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(openVmmId);
        LxssWriteWslConfig(BackendConfig(false));
        const auto hcsId = ReadVmId();
        VerifyBackend(hcsId, false);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(hcsId);
    }
#endif
};
