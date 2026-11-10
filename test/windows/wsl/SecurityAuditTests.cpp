// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "SubProcess.h"
#include "WslSecurity.h"
#include "registry.hpp"
#include "wslsecurityaudit.h"
#pragma push_macro("_NTDEF_")
#define _NTDEF_
#include <ntsecapi.h>
#pragma pop_macro("_NTDEF_")
#include <regex>
#include <nlohmann/json.hpp>

namespace SecurityAuditTests {
namespace {
    constexpr wchar_t c_source[] = L"Microsoft-Windows-WSL-SecurityAudit";
    using unique_event_log = wil::unique_any<HANDLE, decltype(&CloseEventLog), CloseEventLog>;
    using unique_audit_policy = wil::unique_any<PAUDIT_POLICY_INFORMATION, decltype(&AuditFree), AuditFree>;

    struct AuditEvent
    {
        DWORD id;
        std::vector<std::wstring> strings;
    };

    DWORD LatestSecurityRecord()
    {
        unique_event_log log{OpenEventLogW(nullptr, L"Security")};
        THROW_LAST_ERROR_IF(!log);
        DWORD oldest{};
        DWORD count{};
        THROW_IF_WIN32_BOOL_FALSE(GetOldestEventLogRecord(log.get(), &oldest));
        THROW_IF_WIN32_BOOL_FALSE(GetNumberOfEventLogRecords(log.get(), &count));
        return count == 0 ? 0 : oldest + count - 1;
    }

    std::optional<AuditEvent> FindAuditEvent(DWORD after, uint32_t pid)
    {
        unique_event_log log{OpenEventLogW(nullptr, L"Security")};
        THROW_LAST_ERROR_IF(!log);
        std::vector<BYTE> buffer(64 * 1024);
        for (;;)
        {
            DWORD bytes{};
            DWORD needed{};
            if (!ReadEventLogW(
                    log.get(), EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &needed))
            {
                const auto error = GetLastError();
                if (error == ERROR_HANDLE_EOF)
                {
                    return {};
                }
                if (error == ERROR_INSUFFICIENT_BUFFER)
                {
                    buffer.resize(needed);
                    continue;
                }
                THROW_WIN32(error);
            }

            for (DWORD offset = 0; offset < bytes;)
            {
                const auto* record = reinterpret_cast<const EVENTLOGRECORD*>(buffer.data() + offset);
                THROW_HR_IF(E_UNEXPECTED, record->Length < sizeof(*record) || record->Length > bytes - offset);
                if (record->RecordNumber <= after)
                {
                    return {};
                }
                const auto* source = reinterpret_cast<const wchar_t*>(record + 1);
                if (wcscmp(source, c_source) == 0 && record->EventID == MSG_PROCESS_EXECUTED_DETAILS)
                {
                    VERIFY_ARE_EQUAL(static_cast<WORD>(EVENTLOG_AUDIT_SUCCESS), record->EventType);
                    AuditEvent event{record->EventID, {}};
                    const auto* text = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(record) + record->StringOffset);
                    for (WORD index = 0; index < record->NumStrings; ++index)
                    {
                        event.strings.emplace_back(text);
                        text += wcslen(text) + 1;
                    }
                    if (!event.strings.empty() && event.strings.front() == std::to_wstring(pid))
                    {
                        return event;
                    }
                }
                offset += record->Length;
            }
        }
    }

    void VerifyRenderedEvent(const AuditEvent& event, uint32_t pid)
    {
        VERIFY_ARE_EQUAL(size_t{3}, event.strings.size());
        VERIFY_ARE_EQUAL(std::to_wstring(pid), event.strings[0]);
        VERIFY_ARE_EQUAL(wsl::shared::Localization::MessageSecurityAuditProcessExecution(), event.strings[1]);
        const auto details = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(event.strings[2]));
        VERIFY_ARE_EQUAL(1, details.at("schemaVersion").get<int>());
        VERIFY_ARE_EQUAL(pid, details.at("pid").get<uint32_t>());

        const auto key = wsl::windows::common::registry::OpenKey(
            HKEY_LOCAL_MACHINE, std::format(L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Security\\{}", c_source).c_str(), KEY_READ);
        const auto messageFile = wsl::windows::common::registry::ReadString(key.get(), nullptr, L"EventMessageFile");
        wil::unique_hmodule module{LoadLibraryExW(messageFile.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
        THROW_LAST_ERROR_IF(!module);
        DWORD_PTR arguments[] = {
            reinterpret_cast<DWORD_PTR>(event.strings[0].c_str()),
            reinterpret_cast<DWORD_PTR>(event.strings[1].c_str()),
            reinterpret_cast<DWORD_PTR>(event.strings[2].c_str())};
        wil::unique_hlocal_string rendered;
        THROW_LAST_ERROR_IF(
            FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                module.get(),
                event.id,
                0,
                reinterpret_cast<PWSTR>(rendered.put()),
                0,
                reinterpret_cast<va_list*>(arguments)) == 0);
        VERIFY_ARE_EQUAL(event.strings[1] + L"\r\n" + event.strings[2] + L"\r\n", std::wstring(rendered.get()));
    }

    std::pair<uint32_t, bool> ExecuteInVm(HANDLE input, HANDLE output, HANDLE process)
    {
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        const auto marker = std::format("AUDIT_{:08x}_{:04x}_{:04x}", id.Data1, id.Data2, id.Data3);
        // The debug shell runs in the VM's root PID namespace, unlike distribution shells.
        const auto script = std::format(
            "\nsh -c 'printf \"\\n{}_PID=%s\\n\" \"$$\"; exec /bin/true'\n"
            "if pgrep -f '^wsl-audit-collector ' >/dev/null; then printf '\\n%s_RUNNING\\n' '{}'; fi\n"
            "printf '\\n%s_DONE\\n' '{}'\n",
            marker,
            marker,
            marker);
        DWORD written{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(input, script.data(), static_cast<DWORD>(script.size()), &written, nullptr));
        VERIFY_ARE_EQUAL(script.size(), static_cast<size_t>(written));

        std::string result;
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (result.find(marker + "_DONE") == std::string::npos && std::chrono::steady_clock::now() < deadline)
        {
            DWORD available{};
            if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr))
            {
                const auto error = GetLastError();
                LogError("Debug shell output before pipe failure: %hs", result.c_str());
                THROW_WIN32(error);
            }
            if (available != 0)
            {
                char buffer[4096];
                DWORD bytes{};
                THROW_IF_WIN32_BOOL_FALSE(ReadFile(output, buffer, std::min(available, static_cast<DWORD>(sizeof(buffer))), &bytes, nullptr));
                result.append(buffer, bytes);
            }
            else
            {
                THROW_HR_IF(E_UNEXPECTED, WaitForSingleObject(process, 0) != WAIT_TIMEOUT);
                std::this_thread::sleep_for(20ms);
            }
        }
        std::smatch match;
        if (result.find(marker + "_DONE") == std::string::npos || !std::regex_search(result, match, std::regex(marker + "_PID=([0-9]+)")))
        {
            LogError("Incomplete VM audit probe output: %hs", result.c_str());
            VERIFY_FAIL(L"VM audit probe did not complete");
        }
        const auto pid = std::stoull(match[1].str());
        VERIFY_IS_TRUE(pid != 0 && pid <= UINT32_MAX);
        return {static_cast<uint32_t>(pid), result.find(marker + "_RUNNING") != std::string::npos};
    }

    void VerifyAuditPolicy(bool processCreation, bool applicationGenerated)
    {
        wil::unique_handle processToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        wil::unique_handle token;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            processToken.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token));
        SetLastError(ERROR_SUCCESS);
        wsl::windows::common::security::EnableTokenPrivilege(token.get(), SE_SECURITY_NAME);
        THROW_IF_WIN32_ERROR(GetLastError());
        auto impersonation = wil::impersonate_token(token.get());

        GUID categories[] = {
            {0x0cce922b, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}},
            {0x0cce9222, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}}};
        unique_audit_policy original;
        THROW_LAST_ERROR_IF(!AuditQuerySystemPolicy(categories, ARRAYSIZE(categories), &original));
        for (size_t index = 0; index < ARRAYSIZE(categories); ++index)
        {
            // Zero means "leave unchanged" to AuditSetSystemPolicy, not "disable auditing".
            if (original.get()[index].AuditingInformation == POLICY_AUDIT_EVENT_UNCHANGED)
            {
                original.get()[index].AuditingInformation = POLICY_AUDIT_EVENT_NONE;
            }
        }
        auto restore = wil::scope_exit_log(
            WI_DIAGNOSTICS_INFO, [&] { VERIFY_IS_TRUE(AuditSetSystemPolicy(original.get(), ARRAYSIZE(categories)) != FALSE); });
        auto shutdown = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { WslShutdown(); });
        WslShutdown();

        AUDIT_POLICY_INFORMATION policy[] = {original.get()[0], original.get()[1]};
        policy[0].AuditingInformation = processCreation ? POLICY_AUDIT_EVENT_SUCCESS : POLICY_AUDIT_EVENT_NONE;
        policy[1].AuditingInformation = applicationGenerated ? POLICY_AUDIT_EVENT_SUCCESS : POLICY_AUDIT_EVENT_NONE;
        THROW_LAST_ERROR_IF(!AuditSetSystemPolicy(policy, ARRAYSIZE(policy)));

        const auto after = LatestSecurityRecord();
        WslKeepAlive keepAlive;
        auto [input, write] = CreateSubprocessPipe(true, false);
        auto [read, output] = CreateSubprocessPipe(false, true);
        wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        wsl::windows::common::SubProcess shell(nullptr, LxssGenerateWslCommandLine(L"--debug-shell").c_str());
        shell.SetStdHandles(input.get(), output.get(), output.get());
        shell.SetJobObject(job.get());
        const auto process = shell.Start();
        auto stop = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            job.reset();
            VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(process.get(), 5000));
        });
        input.reset();
        output.reset();

        const bool enabled = processCreation && applicationGenerated;
        const auto deadline = std::chrono::steady_clock::now() + (enabled ? 30s : 3s);
        do
        {
            const auto [pid, collecting] = ExecuteInVm(write.get(), read.get(), process.get());
            LogInfo("Audit probe: VM pid=%u, collector=%d", pid, collecting);
            if (!enabled)
            {
                VERIFY_IS_FALSE(collecting);
            }
            std::this_thread::sleep_for(250ms);
            const auto event = FindAuditEvent(after, pid);
            if (enabled && event)
            {
                VERIFY_IS_TRUE(collecting);
                VerifyRenderedEvent(*event, pid);
                return;
            }
            VERIFY_IS_FALSE(!enabled && event.has_value());
        } while (std::chrono::steady_clock::now() < deadline);
        VERIFY_IS_FALSE(enabled, L"No matching WSL process execution event was written to the Security log");
    }
} // namespace

class SecurityAuditTests
{
    WSL_TEST_CLASS(SecurityAuditTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return LxsstuInitialize(FALSE);
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }

    WSL2_TEST_METHOD(ProcessExecutionIsAudited)
    {
        VerifyAuditPolicy(true, true);
    }

    WSL2_TEST_METHOD(ProcessCreationPolicyDisabled)
    {
        VerifyAuditPolicy(false, true);
    }

    WSL2_TEST_METHOD(ApplicationGeneratedPolicyDisabled)
    {
        VerifyAuditPolicy(true, false);
    }
};
} // namespace SecurityAuditTests
