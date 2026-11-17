// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "PluginTests.h"
#include "SecurityAudit.h"
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
        VERIFY_ARE_EQUAL(size_t{4}, event.strings.size());
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
            reinterpret_cast<DWORD_PTR>(event.strings[2].c_str()),
            reinterpret_cast<DWORD_PTR>(event.strings[3].c_str())};
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
        VERIFY_ARE_EQUAL(event.strings[1] + L"\r\n\r\n" + event.strings[3] + L"\r\n", std::wstring(rendered.get()));
    }

    void VerifyAuditPolicy(bool processCreation, bool applicationGenerated)
    {
        wil::unique_handle processToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        wil::unique_handle token;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            processToken.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token));
        wsl::windows::common::security::EnableTokenPrivilege(token.get(), SE_SECURITY_NAME);
        // A successful BOOL result does not guarantee that the privilege was assigned.
        THROW_IF_WIN32_ERROR(GetLastError());
        auto impersonation = wil::impersonate_token(token.get());

        GUID categories[] = {
            wsl::windows::common::securityaudit::c_processCreationAuditSubcategory,
            wsl::windows::common::securityaudit::c_applicationGeneratedAuditSubcategory};
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
        StopWslService();
        auto restart = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { RestartWslService(); });
        auto restore = wil::scope_exit_log(
            WI_DIAGNOSTICS_INFO, [&] { VERIFY_IS_TRUE(AuditSetSystemPolicy(original.get(), ARRAYSIZE(categories)) != FALSE); });

        AUDIT_POLICY_INFORMATION policy[] = {original.get()[0], original.get()[1]};
        policy[0].AuditingInformation = processCreation ? POLICY_AUDIT_EVENT_SUCCESS : POLICY_AUDIT_EVENT_NONE;
        policy[1].AuditingInformation = applicationGenerated ? POLICY_AUDIT_EVENT_SUCCESS : POLICY_AUDIT_EVENT_NONE;
        THROW_LAST_ERROR_IF(!AuditSetSystemPolicy(policy, ARRAYSIZE(policy)));

        const bool enabled = processCreation && applicationGenerated;
        const size_t probeCount = enabled ? 120 : 12;
        const auto command = std::format(
            L"set -eu\n"
            L"i=0\n"
            L"while [ \"$i\" -lt {} ]; do\n"
            L"  sh -c 'printf \"AUDIT_PID=%s\\n\" \"$$\"; exec /bin/true'\n"
            L"  if pgrep -f '^wsl-audit-collector ' >/dev/null; then printf 'AUDIT_COLLECTING\\n'; fi\n"
            L"  i=$((i + 1))\n"
            L"  sleep 0.25\n"
            L"done\n"
            L"printf 'AUDIT_DONE\\n'\n",
            probeCount);

        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        const auto logFile =
            std::filesystem::temp_directory_path() / std::format(L"wsl-audit-{}.log", wsl::shared::string::GuidToString<wchar_t>(id));
        auto deleteLog = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove(logFile); });
        const auto plugin = std::filesystem::path(wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get()).parent_path() /
                            L"testplugin.dll";
        VERIFY_IS_TRUE(std::filesystem::exists(plugin));

        RegistryKeyChange pluginRegistration(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins", L"TestPlugin", plugin.wstring());
        RegistryKeyChange testType(HKEY_LOCAL_MACHINE, c_configKey, c_testType, static_cast<DWORD>(PluginTestType::RunVmCommand));
        RegistryKeyChange pluginLog(HKEY_LOCAL_MACHINE, c_configKey, c_logFile, logFile.wstring());
        RegistryKeyChange vmCommand(HKEY_LOCAL_MACHINE, c_configKey, c_vmCommand, command);
        auto stop = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { StopWslService(); });

        const auto after = LatestSecurityRecord();
        RestartWslService();
        {
            // The plugin probes the root PID namespace without occupying the watchdog's debug console.
            WslKeepAlive keepAlive;
        }
        StopWslService();
        stop.release();

        const auto output = wsl::shared::string::ReadFile<char>(logFile.c_str());
        LogInfo("Audit plugin output: %hs", output.c_str());
        VERIFY_IS_TRUE(output.find("AUDIT_DONE") != std::string::npos);
        VERIFY_ARE_EQUAL(enabled, output.find("AUDIT_COLLECTING") != std::string::npos);

        const std::regex pattern("AUDIT_PID=([0-9]+)");
        std::vector<uint32_t> pids;
        for (auto match = std::sregex_iterator(output.begin(), output.end(), pattern); match != std::sregex_iterator(); ++match)
        {
            const auto pid = std::stoull((*match)[1].str());
            VERIFY_IS_TRUE(pid != 0 && pid <= UINT32_MAX);
            pids.push_back(static_cast<uint32_t>(pid));
        }
        VERIFY_ARE_EQUAL(probeCount, pids.size());

        for (const auto pid : pids)
        {
            const auto event = FindAuditEvent(after, pid);
            if (enabled && event)
            {
                VerifyRenderedEvent(*event, pid);
                return;
            }
            VERIFY_IS_FALSE(!enabled && event.has_value());
        }
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
