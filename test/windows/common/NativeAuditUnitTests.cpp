// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include <future>
#include <msiquery.h>
#include "Common.h"
#include "NativeAudit.h"
#include "NativeAuditLaunch.h"
#include "WindowsSecuritySink.h"
#include "wslc.h"

using namespace wsl::audit;

namespace {
constexpr ULONG c_successAudit = 1;
constexpr ULONG c_failureAudit = 2;
using UniqueMsiHandle = wil::unique_any<MSIHANDLE, decltype(MsiCloseHandle), &MsiCloseHandle>;

std::wstring ReadMsiString(MSIHANDLE record, UINT field)
{
    wchar_t empty{};
    DWORD length{};
    const auto result = MsiRecordGetStringW(record, field, &empty, &length);
    THROW_IF_WIN32_ERROR(result == ERROR_MORE_DATA ? ERROR_SUCCESS : result);
    std::wstring value(length + 1, L'\0');
    ++length;
    THROW_IF_WIN32_ERROR(MsiRecordGetStringW(record, field, value.data(), &length));
    value.resize(length);
    return value;
}

class DrainedAudit : public NativeAudit
{
public:
    using NativeAudit::NativeAudit;
    ~DrainedAudit()
    {
        Stop();
        if (WorkerHandle())
        {
            VERIFY_ARE_EQUAL(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(WorkerHandle(), 5000));
        }
    }
};

class MemorySink final : public AuditSink
{
public:
    void Write(const nlohmann::json& message) override
    {
        std::lock_guard lock(m_lock);
        m_messages.push_back(message);
    }
    void Flush() override
    {
    }
    std::mutex m_lock;
    std::vector<nlohmann::json> m_messages;
};

nlohmann::json ProcessEvent()
{
    return {
        {"version", c_protocolVersion},
        {"kind", "process"},
        {"pid", 1U},
        {"tid", 1U},
        {"ppid", 0U},
        {"pidNamespace", 42U},
        {"mountNamespace", 43U},
        {"uid", 0U},
        {"euid", 0U},
        {"gid", 0U},
        {"egid", 0U},
        {"timestampNs", uint64_t{123}},
        {"startTimeNs", uint64_t{100}},
        {"executable", "/bin/echo"},
        {"cwd", "/"},
        {"cmdlineTruncated", false},
        {"argv", {"echo", "sensitive-marker"}}};
}

nlohmann::json Ready(const AuditPolicy& policy)
{
    return {{"version", c_protocolVersion}, {"kind", "ready"}, {"status", "running"}, {"pidNamespace", 41U}, {"policy", PolicySettings(policy)}};
}

HostIdentity Host(bool wslc = false)
{
    HostIdentity host{"S-1-5-21-1001-1002-1003-1004", 7U, 2U};
    if (wslc)
    {
        host.wslSessionId.reset();
        host.wslcSessionId = 7U;
        host.wslcSessionName = "containers";
    }
    return host;
}

NativeAuditServices Services(const std::shared_ptr<MemorySink>& sink)
{
    return {[] { return true; }, [] { return AuditPolicy{true, false, "windows"}; }, [sink](const auto&) { return sink; }};
}

AuditTransport ScriptedTransport(std::string_view script, std::vector<UniqueSocket>& peers, bool closeOutput = true)
{
    UniqueSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), !listener);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0);
    int length = sizeof(address);
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0);
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), listen(listener.get(), 1) != 0);
    AuditTransport transport;
    transport.output.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), connect(transport.output.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0);
    UniqueSocket peer(accept(listener.get(), nullptr, nullptr));
    THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), !peer);
    SendAll(peer.get(), script);
    if (closeOutput)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), shutdown(peer.get(), SD_SEND) != 0);
    }
    peers.emplace_back(std::move(peer));
    return transport;
}
} // namespace

class NativeAuditUnitTests
{
    WSL_TEST_CLASS(NativeAuditUnitTests)

    TEST_METHOD(DefaultOnAndAdministratorOnlyDisable)
    {
        VERIFY_IS_TRUE(MachineAuditEnabled(std::nullopt));
        VERIFY_IS_TRUE(MachineAuditEnabled(RegistryPolicyValue{REG_DWORD, 1}));
        VERIFY_IS_FALSE(MachineAuditEnabled(RegistryPolicyValue{REG_DWORD, 0}));
        VERIFY_IS_TRUE(MachineAuditEnabled(RegistryPolicyValue{REG_SZ, 0}));
        VERIFY_IS_TRUE(MachineAuditEnabled(RegistryPolicyValue{REG_DWORD, 99}));
    }

    TEST_METHOD(OnlyTheLegacyAuditPluginIsSuppressed)
    {
        VERIFY_IS_TRUE(IsLegacyAuditPlugin(L"WSL-SecurityAudit-Prototype"));
        VERIFY_IS_TRUE(IsLegacyAuditPlugin(L"wsl-securityaudit-prototype"));
        VERIFY_IS_FALSE(IsLegacyAuditPlugin(L"MDE"));
        VERIFY_IS_FALSE(IsLegacyAuditPlugin(L"WSL-SecurityAudit-Prototype-Other"));
        VERIFY_IS_FALSE(IsLegacyAuditPlugin(L""));
    }

    TEST_METHOD(AdministratorDisableDoesNotLaunchCollector)
    {
        struct State
        {
            wil::unique_event queried{wil::EventOptions::ManualReset};
            std::atomic<unsigned> launches{};
        };
        auto state = std::make_shared<State>();
        auto services = Services(std::make_shared<MemorySink>());
        services.enabled = [state] {
            state->queried.SetEvent();
            return false;
        };
        DrainedAudit audit(std::move(services));
        audit.Start(Host(), [state](const auto&) -> AuditTransport {
            ++state->launches;
            THROW_HR(E_UNEXPECTED);
        });
        VERIFY_IS_TRUE(state->queried.wait(5000));
        VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == NativeAudit::StartupResult::Disabled);
        audit.Stop();
        VERIFY_ARE_EQUAL(0U, state->launches.load());
    }

    TEST_METHOD(AuditSubcategoriesRemainAuthoritative)
    {
        VERIFY_IS_FALSE(EvaluatePolicy({}, RegistryPolicyValue{REG_DWORD, 1}).AnyEnabled());
        const auto failures = EvaluatePolicy({c_failureAudit, c_failureAudit, 0, 0}, {});
        VERIFY_IS_FALSE(failures.processCreation);
        VERIFY_IS_FALSE(failures.includeCommandLine);
        VERIFY_IS_TRUE(failures.networkFailure);
        VERIFY_IS_FALSE(failures.networkSuccess);
        const auto successes = EvaluatePolicy({c_successAudit, 0, 0, 0}, {});
        VERIFY_IS_TRUE(successes.processCreation);
        VERIFY_IS_FALSE(successes.includeCommandLine);
        VERIFY_THROWS(EvaluatePolicy({}, RegistryPolicyValue{REG_SZ, 1}), std::invalid_argument);
        const AuditPolicy all{true, true, "windows", true, true, true, true};
        VERIFY_IS_FALSE(AllowsSecurityEvent(all, "namespace-change", "success"));
        VERIFY_IS_FALSE(AllowsSecurityEvent(all, "time-change", "success"));
        const auto unsupported = LibraryCoverage()["privilege-transition"]["unsupported"];
        VERIFY_IS_TRUE(std::find(unsupported.begin(), unsupported.end(), "capset") != unsupported.end());
    }

    TEST_METHOD(StartupExitAndMalformedFrames)
    {
        const AuditPolicy policy{true, false, "windows"};
        auto sink = std::make_shared<MemorySink>();
        EventSession session(policy, Host(), sink);
        VERIFY_THROWS(session.Accept(ProcessEvent()), std::invalid_argument);
        VERIFY_THROWS(session.Finish(), std::runtime_error);
        session.Accept(Ready(policy));
        VERIFY_THROWS(session.Accept(Ready(policy)), std::invalid_argument);
        auto malformed = ProcessEvent();
        malformed["pid"] = -1;
        VERIFY_THROWS(session.Accept(malformed), ProtocolError);
        VERIFY_THROWS(session.Accept({{"kind", "error"}, {"message", "secret"}}), std::runtime_error);
        session.Accept({{"version", c_protocolVersion}, {"kind", "stopped"}, {"exitCode", 0U}});
        VERIFY_NO_THROW(session.Finish());
        VERIFY_THROWS(session.Accept(ProcessEvent()), std::invalid_argument);
        NdjsonReceiver receiver;
        VERIFY_THROWS(receiver.Feed("{\"version\":1,\"kind\":\"ready\"}\n", [](auto) {}), ProtocolError);
        receiver.Feed("{", [](auto) {});
        VERIFY_THROWS(receiver.Finish(), ProtocolError);
    }

    TEST_METHOD(WslAndWslcIdentityDoNotCollide)
    {
        const AuditPolicy policy{true, false, "windows"};
        const DistributionIdentity distro{"distro", "{12345678-1234-5678-abcd-123456789abc}", 42};
        auto event = ProcessEvent();
        event["windowsUserSid"] = "S-1-5-18";
        event["wslcSessionId"] = 99U;
        event["containerId"] = "forged";
        auto ordinary = ApplyWindowsPolicy(event, policy, Host(), {distro});
        VERIFY_IS_TRUE(ordinary["attribution"] == "matched");
        VERIFY_IS_FALSE(ordinary.contains("wslcSessionId"));
        VERIFY_IS_FALSE(ordinary.contains("containerId"));
        VERIFY_IS_FALSE(ordinary.contains("argv"));
        VERIFY_IS_TRUE(ordinary["windowsUserSid"] == Host().sid);
        VERIFY_IS_TRUE(ApplyWindowsPolicy(event, policy, Host(), {distro, distro})["distroId"].is_null());
        const ContainerIdentity container{"container", "name", "2026-10-22T00:00:00Z", 1, 42, 43, 100};
        auto wslc = ApplyWindowsPolicy(event, policy, Host(true), {distro}, {container}, 41);
        VERIFY_IS_TRUE(wslc["wslSessionId"].is_null());
        VERIFY_IS_TRUE(wslc["distroId"].is_null());
        VERIFY_IS_TRUE(wslc["wslcSessionId"] == 7U);
        VERIFY_IS_TRUE(wslc["containerId"] == "container");
        event["timestampNs"] = 99U;
        VERIFY_IS_TRUE(ApplyWindowsPolicy(event, policy, Host(true), {}, {container}, 41)["containerId"].is_null());
        event["timestampNs"] = 123U;
        VERIFY_IS_TRUE(ApplyWindowsPolicy(event, policy, Host(true), {}, {}, 41)["containerId"].is_null());
        VERIFY_IS_TRUE(ApplyWindowsPolicy(event, policy, Host(true), {}, {container, container}, 41)["containerId"].is_null());
    }

    TEST_METHOD(NativeSourceNeverClaimsLegacyOwnership)
    {
        VERIFY_NO_THROW(ValidateSecuritySourceName(c_securitySourceName));
        VERIFY_THROWS(ValidateSecuritySourceName(L"WSL-SecurityAudit"), wil::ResultException);
        security_detail::SourceInfo source;
        source.installed = true;
        source.registryStatus = source.registryMessageStatus = ERROR_SUCCESS;
        source.messageFile = L"C:\\Program Files\\WSL\\wslsecurityaudit.dll";
        source.registryMessageFile = source.messageFile;
        VERIFY_IS_TRUE(security_detail::SourceReadbackMatches(source, source.messageFile));
        VERIFY_IS_FALSE(security_detail::SourceReadbackMatches(source, L"C:\\other.dll"));
        source.registryMessageFile = L"C:\\old-prototype.dll";
        VERIFY_IS_FALSE(security_detail::SourceReadbackMatches(source));
    }

    TEST_METHOD(BuiltMsiRegistersNativeAuditProcessFactory)
    {
        const auto modulePath = std::filesystem::path(wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle()));
        const auto msiPath = modulePath.parent_path() / L"wsl.msi";
        UniqueMsiHandle database;
        VERIFY_ARE_EQUAL(UINT{ERROR_SUCCESS}, MsiOpenDatabaseW(msiPath.c_str(), MSIDBOPEN_READONLY, &database), msiPath.c_str());

        wchar_t iid[39]{};
        VERIFY_ARE_EQUAL(39, StringFromGUID2(__uuidof(IWSLCNativeAuditProcessFactory), iid, ARRAYSIZE(iid)));
        const auto key = std::wstring(L"Interface\\") + iid + L"\\ProxyStubClsid32";
        UniqueMsiHandle parameters{MsiCreateRecord(1)};
        VERIFY_IS_TRUE(parameters.is_valid());
        VERIFY_ARE_EQUAL(UINT{ERROR_SUCCESS}, MsiRecordSetStringW(parameters.get(), 1, key.c_str()));
        UniqueMsiHandle view;
        VERIFY_ARE_EQUAL(
            UINT{ERROR_SUCCESS},
            MsiDatabaseOpenViewW(database.get(), L"SELECT `Root`, `Name`, `Value`, `Component_` FROM `Registry` WHERE `Key` = ?", &view));
        VERIFY_ARE_EQUAL(UINT{ERROR_SUCCESS}, MsiViewExecute(view.get(), parameters.get()));
        UniqueMsiHandle row;
        VERIFY_ARE_EQUAL(UINT{ERROR_SUCCESS}, MsiViewFetch(view.get(), &row));
        VERIFY_ARE_EQUAL(0, MsiRecordGetInteger(row.get(), 1));
        VERIFY_IS_TRUE(ReadMsiString(row.get(), 2).empty());
        VERIFY_ARE_EQUAL(std::wstring(L"{4EA0C6DD-E9FF-48E7-994E-13A31D10DC60}"), ReadMsiString(row.get(), 3));
        VERIFY_ARE_EQUAL(std::wstring(L"wslservice"), ReadMsiString(row.get(), 4));
        row.reset();
        VERIFY_ARE_EQUAL(UINT{ERROR_NO_MORE_ITEMS}, MsiViewFetch(view.get(), &row));
        VERIFY_ARE_EQUAL(UINT{ERROR_SUCCESS}, MsiViewClose(view.get()));
    }

    TEST_METHOD(CollectorStartupRetriesAreFinite)
    {
        struct Attempts
        {
            std::atomic<unsigned> count{};
            wil::unique_event exhausted{wil::EventOptions::ManualReset};
        };
        auto attempts = std::make_shared<Attempts>();
        auto sink = std::make_shared<MemorySink>();
        DrainedAudit audit(Services(sink));
        audit.Start(Host(), [attempts](const auto&) -> AuditTransport {
            if (++attempts->count == c_maxCollectorAttempts)
            {
                attempts->exhausted.SetEvent();
            }
            THROW_HR(E_FAIL);
        });
        VERIFY_IS_TRUE(attempts->exhausted.wait(10000));
        audit.Stop();
        VERIFY_ARE_EQUAL(c_maxCollectorAttempts, attempts->count.load());
    }

    TEST_METHOD(StopDoesNotJoinAndNewEpochIsIndependent)
    {
        struct Gate
        {
            wil::unique_event entered{wil::EventOptions::ManualReset};
            wil::unique_event release{wil::EventOptions::ManualReset};
            wil::unique_event retired{wil::EventOptions::ManualReset};
        };
        auto gate = std::make_shared<Gate>();
        auto sink = std::make_shared<MemorySink>();
        DrainedAudit audit(Services(sink));
        const auto cleanup = wil::scope_exit([&] { gate->release.SetEvent(); });
        audit.Start(Host(), [gate](const auto&) -> AuditTransport {
            gate->entered.SetEvent();
            gate->release.wait(10000);
            gate->retired.SetEvent();
            THROW_HR(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        });
        VERIFY_IS_TRUE(gate->entered.wait(5000));
        wil::unique_handle oldWorker;
        VERIFY_WIN32_BOOL_SUCCEEDED(
            DuplicateHandle(GetCurrentProcess(), audit.WorkerHandle(), GetCurrentProcess(), &oldWorker, SYNCHRONIZE, FALSE, 0));
        const auto started = GetTickCount64();
        audit.Stop();
        VERIFY_IS_TRUE(GetTickCount64() - started < 1000);
        auto next = std::make_shared<Gate>();
        audit.Start(Host(true), [next](const auto&) -> AuditTransport {
            next->entered.SetEvent();
            THROW_HR(E_FAIL);
        });
        VERIFY_IS_TRUE(next->entered.wait(5000));
        audit.Stop();
        gate->release.SetEvent();
        VERIFY_IS_TRUE(gate->retired.wait(5000));
        VERIFY_ARE_EQUAL(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(oldWorker.get(), 5000));
    }

    TEST_METHOD(CollectorLossAndMalformedStreamRetry)
    {
        for (const bool malformed : {false, true})
        {
            struct State
            {
                std::atomic<unsigned> attempts{};
                wil::unique_event exhausted{wil::EventOptions::ManualReset};
                std::vector<UniqueSocket> peers;
            };
            auto state = std::make_shared<State>();
            auto sink = std::make_shared<MemorySink>();
            DrainedAudit audit(Services(sink));
            const auto script = malformed ? std::string("{invalid}\n") : EncodeMessage(Ready({true, false, "windows"}));
            audit.Start(Host(), [state, script](const auto&) {
                auto result = ScriptedTransport(script, state->peers);
                if (++state->attempts == c_maxCollectorAttempts)
                {
                    state->exhausted.SetEvent();
                }
                return result;
            });
            VERIFY_IS_TRUE(state->exhausted.wait(10000));
            VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == (malformed ? NativeAudit::StartupResult::Failed : NativeAudit::StartupResult::Ready));
            audit.Stop();
            VERIFY_ARE_EQUAL(c_maxCollectorAttempts, state->attempts.load());
        }
    }

    TEST_METHOD(SlowLaunchStopAndReplacementVmUseIndependentCapabilities)
    {
        struct Vm
        {
            GUID id{};
            wil::unique_event terminating{wil::EventOptions::ManualReset};
            Vm()
            {
                THROW_IF_FAILED(CoCreateGuid(&id));
            }
            ~Vm()
            {
                terminating.SetEvent();
            }
        };
        struct Gate
        {
            wil::unique_event entered{wil::EventOptions::ManualReset};
            wil::unique_event cancelled{wil::EventOptions::ManualReset};
            wil::unique_event release{wil::EventOptions::ManualReset};
            wil::unique_event replacementEntered{wil::EventOptions::ManualReset};
            std::atomic<bool> oldIdentityMatched{};
            std::atomic<bool> newIdentityMatched{};
        };
        auto gate = std::make_shared<Gate>();
        auto oldVm = std::make_unique<Vm>();
        const auto oldId = oldVm->id;
        const wsl::windows::common::NativeAuditLaunch oldLaunch(oldId, oldVm->terminating.get());
        DrainedAudit audit(Services(std::make_shared<MemorySink>()));
        const auto cleanup = wil::scope_exit([&] { gate->release.SetEvent(); });
        audit.Start(Host(), [oldLaunch, oldId, gate](const auto& arguments) -> AuditTransport {
            AuditTransport transport;
            transport.output =
                oldLaunch.CreateRootNamespaceProcess(arguments, [oldId, gate](const GUID& id, ULONG, HANDLE stop, DWORD) -> UniqueSocket {
                    gate->oldIdentityMatched = id == oldId;
                    gate->entered.SetEvent();
                    if (WaitForSingleObject(stop, 5000) == WAIT_OBJECT_0)
                    {
                        gate->cancelled.SetEvent();
                    }
                    gate->release.wait(5000);
                    THROW_HR(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                });
            return transport;
        });
        VERIFY_IS_TRUE(gate->entered.wait(5000));
        wil::unique_handle oldWorker;
        VERIFY_WIN32_BOOL_SUCCEEDED(
            DuplicateHandle(GetCurrentProcess(), audit.WorkerHandle(), GetCurrentProcess(), &oldWorker, SYNCHRONIZE, FALSE, 0));

        const auto start = GetTickCount64();
        audit.Stop();
        oldVm.reset();
        VERIFY_IS_TRUE(GetTickCount64() - start < 1000);
        VERIFY_IS_TRUE(gate->cancelled.wait(1000));
        VERIFY_IS_FALSE(gate->release.is_signaled());

        Vm replacement;
        const auto replacementId = replacement.id;
        const wsl::windows::common::NativeAuditLaunch replacementLaunch(replacementId, replacement.terminating.get());
        audit.Start(Host(true), [replacementLaunch, replacementId, oldId, gate](const auto& arguments) -> AuditTransport {
            AuditTransport transport;
            transport.output = replacementLaunch.CreateRootNamespaceProcess(
                arguments, [replacementId, oldId, gate](const GUID& id, ULONG, HANDLE, DWORD) -> UniqueSocket {
                    gate->newIdentityMatched = id == replacementId && id != oldId;
                    gate->replacementEntered.SetEvent();
                    THROW_HR(E_FAIL);
                });
            return transport;
        });
        VERIFY_IS_TRUE(gate->replacementEntered.wait(1000));
        VERIFY_IS_TRUE(gate->oldIdentityMatched.load());
        VERIFY_IS_TRUE(gate->newIdentityMatched.load());
        audit.Stop();
        gate->release.SetEvent();
        VERIFY_ARE_EQUAL(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(oldWorker.get(), 5000));
    }

    TEST_METHOD(MissingDnsUprobesAreExplicitWithoutInventingCoverage)
    {
        const AuditPolicy policy{true, false, "windows", true, true};
        auto ready = Ready(policy);
        ready["dnsProbeCount"] = 0U;
        ready["dnsCoverage"] = "unavailable";
        ready["initializationDiagnostics"] = true;
        ready["coverage"] = ExpectedCollectorCoverage(ready);
        auto sink = std::make_shared<MemorySink>();
        EventSession session(policy, Host(), sink);
        VERIFY_NO_THROW(session.Accept(ready));
        const auto diagnostic = ProjectSecurityDiagnostic(sink->m_messages.back(), policy);
        VERIFY_IS_TRUE(diagnostic.has_value());
        VERIFY_IS_TRUE(diagnostic->at("dnsCoverage") == "unavailable");
        VERIFY_IS_TRUE(diagnostic->at("initializationDiagnostics") == true);
        const auto supported = diagnostic->at("coverage").at("network").at("supported");
        VERIFY_IS_TRUE(std::find(supported.begin(), supported.end(), "dns-query") == supported.end());
        VERIFY_IS_TRUE(std::find(supported.begin(), supported.end(), "socket") != supported.end());
        ready["dnsCoverage"] = "vm-root-only";
        VERIFY_THROWS(ExpectedCollectorCoverage(ready), ProtocolError);
    }

    TEST_METHOD(StartupGateReleasesAfterValidatedReady)
    {
        struct State
        {
            std::vector<UniqueSocket> peers;
        };
        auto state = std::make_shared<State>();
        auto sink = std::make_shared<MemorySink>();
        DrainedAudit audit(Services(sink));
        audit.Start(Host(), [state](const auto&) {
            return ScriptedTransport(EncodeMessage(Ready({true, false, "windows"})), state->peers, false);
        });
        const auto startup = audit.CaptureStartupWait();
        VERIFY_IS_TRUE(startup.Wait() == NativeAudit::StartupResult::Ready);
        const auto start = GetTickCount64();
        VERIFY_IS_TRUE(startup.Wait() == NativeAudit::StartupResult::Ready);
        VERIFY_IS_TRUE(GetTickCount64() - start < 1000);
        const auto messages = [&] {
            std::lock_guard lock(sink->m_lock);
            return sink->m_messages;
        }();
        VERIFY_ARE_EQUAL(size_t{2}, messages.size());
        const auto& awaiting = messages.at(0);
        const auto& ready = messages.at(1);
        VERIFY_IS_TRUE(awaiting.at("kind") == "windows-status" && awaiting.at("status") == "awaiting-ready");
        VERIFY_IS_TRUE(ready.at("kind") == "ready" && ready.at("status") == "running");
        VERIFY_IS_TRUE(
            ready.at("pidNamespace") == 41U && ready.at("policy") == PolicySettings(AuditPolicy{true, false, "windows"}));
        VERIFY_IS_TRUE(ready.at("captureSessionId") == awaiting.at("captureSessionId"));
    }

    TEST_METHOD(DisabledPolicyReleasesStartupGateWithoutLaunching)
    {
        auto launches = std::make_shared<std::atomic<unsigned>>(0);
        auto services = Services(std::make_shared<MemorySink>());
        services.policy = [] { return AuditPolicy{}; };
        DrainedAudit audit(std::move(services));
        const auto start = GetTickCount64();
        audit.Start(Host(true), [launches](const auto&) -> AuditTransport {
            ++*launches;
            THROW_HR(E_UNEXPECTED);
        });
        VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == NativeAudit::StartupResult::Disabled);
        VERIFY_IS_TRUE(GetTickCount64() - start < 1000);
        VERIFY_ARE_EQUAL(0U, launches->load());
    }

    TEST_METHOD(StartupFailureReleasesGateAfterFiniteRetries)
    {
        auto launches = std::make_shared<std::atomic<unsigned>>(0);
        DrainedAudit audit(Services(std::make_shared<MemorySink>()));
        const auto start = GetTickCount64();
        audit.Start(Host(), [launches](const auto&) -> AuditTransport {
            ++*launches;
            THROW_HR(E_FAIL);
        });
        VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == NativeAudit::StartupResult::Failed);
        VERIFY_ARE_EQUAL(c_maxCollectorAttempts, launches->load());
        VERIFY_IS_TRUE(GetTickCount64() - start < c_auditStartupTimeoutMs);
    }

    TEST_METHOD(StartupRetriesShareOneOverallDeadline)
    {
        struct State
        {
            std::atomic<unsigned> launches{};
            std::vector<UniqueSocket> peers;
        };
        auto state = std::make_shared<State>();
        auto services = Services(std::make_shared<MemorySink>());
        services.startupTimeoutMs = 1500;
        DrainedAudit audit(std::move(services));
        const auto start = GetTickCount64();
        audit.Start(Host(true), [state](const auto&) -> AuditTransport {
            if (++state->launches == 1)
            {
                THROW_HR(E_FAIL);
            }
            return ScriptedTransport({}, state->peers, false);
        });
        VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == NativeAudit::StartupResult::TimedOut);
        const auto elapsed = GetTickCount64() - start;
        VERIFY_IS_TRUE(elapsed >= 1200 && elapsed < 3000);
        VERIFY_ARE_EQUAL(2U, state->launches.load());
    }

    TEST_METHOD(SlowFactoryCannotExtendStartupWait)
    {
        struct State
        {
            wil::unique_event entered{wil::EventOptions::ManualReset};
            wil::unique_event release{wil::EventOptions::ManualReset};
        };
        auto state = std::make_shared<State>();
        auto services = Services(std::make_shared<MemorySink>());
        services.startupTimeoutMs = 250;
        DrainedAudit audit(std::move(services));
        const auto cleanup = wil::scope_exit([&] { state->release.SetEvent(); });
        audit.Start(Host(), [state](const auto&) -> AuditTransport {
            state->entered.SetEvent();
            state->release.wait(5000);
            THROW_HR(E_FAIL);
        });
        VERIFY_IS_TRUE(state->entered.wait(1000));
        const auto start = GetTickCount64();
        VERIFY_IS_TRUE(audit.CaptureStartupWait().Wait() == NativeAudit::StartupResult::TimedOut);
        VERIFY_IS_TRUE(GetTickCount64() - start < 1500);
        VERIFY_IS_FALSE(state->release.is_signaled());
    }

    TEST_METHOD(CancelledStartupWaitCannotObserveReplacementReady)
    {
        struct State
        {
            wil::unique_event entered{wil::EventOptions::ManualReset};
            wil::unique_event release{wil::EventOptions::ManualReset};
            std::vector<UniqueSocket> peers;
        };
        auto old = std::make_shared<State>();
        auto replacement = std::make_shared<State>();
        DrainedAudit audit(Services(std::make_shared<MemorySink>()));
        const auto cleanup = wil::scope_exit([&] { old->release.SetEvent(); });
        audit.Start(Host(), [old](const auto&) -> AuditTransport {
            old->entered.SetEvent();
            old->release.wait(5000);
            THROW_HR(E_FAIL);
        });
        VERIFY_IS_TRUE(old->entered.wait(1000));
        const auto oldStartup = audit.CaptureStartupWait();
        auto waiting = std::async(std::launch::async, [oldStartup] { return oldStartup.Wait(); });
        wil::unique_handle oldWorker;
        VERIFY_WIN32_BOOL_SUCCEEDED(
            DuplicateHandle(GetCurrentProcess(), audit.WorkerHandle(), GetCurrentProcess(), &oldWorker, SYNCHRONIZE, FALSE, 0));
        audit.Stop();
        VERIFY_IS_TRUE(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        VERIFY_IS_TRUE(waiting.get() == NativeAudit::StartupResult::Cancelled);
        audit.Start(Host(true), [replacement](const auto&) {
            return ScriptedTransport(EncodeMessage(Ready({true, false, "windows"})), replacement->peers, false);
        });
        const auto current = audit.CaptureStartupWait();
        VERIFY_IS_TRUE(current.Wait() == NativeAudit::StartupResult::Ready);
        old->release.SetEvent();
        VERIFY_ARE_EQUAL(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(oldWorker.get(), 5000));
        VERIFY_IS_TRUE(oldStartup.Wait() == NativeAudit::StartupResult::Cancelled);
        VERIFY_IS_TRUE(current.Wait() == NativeAudit::StartupResult::Ready);
    }
};
