// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <iostream>
#include <limits>
#include "Protocol.h"
#include "ProcessExecuted.h"
#include "ProcessCreated.h"
#include "SecurityFixture.h"

using namespace wsl::audit;

namespace {

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callable>
void ExpectRejected(Callable&& callable, const char* message)
{
    bool rejected = false;
    try
    {
        callable();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }

    Expect(rejected, message);
}

nlohmann::json ProcessEvent()
{
    return {
        {"version", c_protocolVersion},
        {"kind", "process"},
        {"pid", 123U},
        {"tid", 123U},
        {"ppid", 42U},
        {"pidNamespace", 4026531836U},
        {"uid", 1000U},
        {"euid", 1000U},
        {"gid", 1000U},
        {"egid", 1000U},
        {"timestampNs", std::numeric_limits<uint64_t>::max()},
        {"startTimeNs", uint64_t{123456789}},
        {"executable", "/usr/bin/echo"},
        {"cwd", "/home/test"},
        {"cmdlineTruncated", false}};
}

void TestSecurityFields()
{
    for (const auto* operation :
         {"process-created",
          "socket",
          "bind",
          "connect",
          "accept",
          "accept4",
          "dns-query",
          "init_module",
          "finit_module",
          "setuid",
          "setgid",
          "setresgid",
          "unshare",
          "setns"})
    {
        auto event = SecurityFixture(operation);
        ValidateSecurityEvent(event);
        const auto restored = ParseMessage(EncodeMessage(event));
        Expect(restored == event, "Security event did not round-trip");
        event["argv"] = {"private-argument"};
        event["windowsUserSid"] = "guest-forgery";
        const auto selected = SelectSecurityEvent(event);
        Expect(!selected.contains("argv") && !selected.contains("windowsUserSid"), "Untrusted extension crossed boundary");
        Expect(selected["timestampNs"] == UINT64_MAX, "Security timestamp narrowed");
    }
    auto failed = SecurityFixture();
    failed["result"] = int64_t{-111};
    failed["outcome"] = "failure";
    ValidateSecurityEvent(failed);
    failed["outcome"] = "success";
    ExpectRejected([&] { ValidateSecurityEvent(failed); }, "Failure mislabeled as success");
    failed["result"] = int64_t{-115};
    failed["outcome"] = "pending";
    ValidateSecurityEvent(failed);
    auto dns = SecurityFixture("dns-query");
    dns["result"] = int64_t{12};
    dns["fields"]["dnsStatus"] = 1U;
    dns["outcome"] = "failure";
    ValidateSecurityEvent(dns);
    auto invalid = SecurityFixture();
    invalid["fields"]["protocol"] = 6U;
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Unavailable protocol was invented");
    invalid = SecurityFixture();
    invalid["fields"]["socketFd"] = "secret";
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Untyped field accepted");
    invalid = SecurityFixture();
    invalid["fields"]["argv"] = {"secret"};
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Unknown nested field accepted");
    invalid = SecurityFixture();
    invalid["uid"] = uint64_t{UINT32_MAX} + 1;
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Credential overflow accepted");
    invalid = SecurityFixture();
    invalid["fields"]["localPort"] = 65536U;
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Port overflow accepted");
    invalid = SecurityFixture();
    invalid["fields"]["dnsName"] = "not-a-correlated-name";
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Uncorrelated DNS name attached to a connection");
    invalid = SecurityFixture();
    invalid["family"] = "module-load";
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Wrong event family accepted");
    invalid = SecurityFixture();
    invalid["operation"] = "clock_settime";
    ExpectRejected([&] { ValidateSecurityEvent(invalid); }, "Unsupported operation accepted");
    Expect(!LibraryCoverage()["complete"].get<bool>(), "Partial library coverage reported complete");
    Expect(LibraryCoverage()["time-change"]["supported"].empty(), "Time support fabricated");
    Expect(LibraryCoverage().size() == 9, "Seven-family coverage contract changed");
}

void TestPolicies()
{
    for (const bool processCreation : {false, true})
    {
        for (const bool includeCommandLine : {false, true})
        {
            const AuditPolicy expected{processCreation, includeCommandLine, "test-override"};
            const auto actual = ParseConfiguration(EncodeMessage(Configuration(expected)));
            Expect(actual.processCreation == expected.processCreation, "Process policy did not round-trip");
            Expect(
                actual.includeCommandLine == (processCreation && includeCommandLine),
                "Command-line policy was not gated by process auditing");
            Expect(actual.source == expected.source, "Policy provenance was lost");
        }
    }

    auto invalid = Configuration({});
    invalid["processCreation"] = "true";
    ExpectRejected([&]() { ParseConfiguration(EncodeMessage(invalid)); }, "String policy accepted as boolean");
    invalid = Configuration({});
    invalid.erase("includeCommandLine");
    ExpectRejected([&]() { ParseConfiguration(EncodeMessage(invalid)); }, "Missing policy flag accepted");
    invalid = Configuration({});
    invalid["policySource"] = "pretend-windows";
    ExpectRejected([&]() { ParseConfiguration(EncodeMessage(invalid)); }, "Unknown policy source accepted");
    invalid = Configuration({});
    invalid["kind"] = "stop";
    ExpectRejected([&]() { ParseConfiguration(EncodeMessage(invalid)); }, "Wrong configuration kind accepted");
    for (const auto* field : {"networkSuccess", "networkFailure", "moduleLoad", "privilegeTransition"})
    {
        invalid = Configuration({});
        invalid.erase(field);
        ExpectRejected([&] { ParseConfiguration(EncodeMessage(invalid)); }, "Missing category policy accepted");
        invalid[field] = 1;
        ExpectRejected([&] { ParseConfiguration(EncodeMessage(invalid)); }, "Nonboolean category policy accepted");
    }
    invalid = Configuration({});
    invalid["includeCommandLine"] = true;
    ExpectRejected([&] { ParseConfiguration(EncodeMessage(invalid)); }, "Arguments enabled without process auditing");
    invalid = Configuration({});
    invalid["namespaceChange"] = true;
    ExpectRejected([&] { ParseConfiguration(EncodeMessage(invalid)); }, "Unknown category silently enabled");
    invalid = Configuration({});
    invalid["version"] = 1U;
    ExpectRejected([&] { ParseConfiguration(EncodeMessage(invalid)); }, "Legacy all-enabled protocol accepted");
    for (unsigned int mask = 0; mask < 32; ++mask)
    {
        const AuditPolicy expected{(mask & 1) != 0, true, "windows", (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0, (mask & 16) != 0};
        const auto actual = ParseConfiguration(EncodeMessage(Configuration(expected)));
        Expect(PolicySettings(actual) == PolicySettings(expected), "Category settings did not round-trip");
        Expect(actual.AnyEnabled() == (mask != 0), "Any-enabled status depends on process policy");
        for (const auto outcome : {"success", "failure", "pending", "unknown"})
        {
            const std::string_view result(outcome);
            const bool network = result == "success"   ? expected.networkSuccess
                                 : result == "failure" ? expected.networkFailure
                                                       : expected.networkSuccess && expected.networkFailure;
            Expect(AllowsSecurityEvent(actual, "network", result) == network, "Network outcome filtering mismatch");
            Expect(
                AllowsSecurityEvent(actual, "process-creation", result) == (expected.processCreation && result == "success"),
                "Creation policy mismatch");
            Expect(
                AllowsSecurityEvent(actual, "module-load", result) == (expected.moduleLoad && result == "success"),
                "Module policy mismatch");
            Expect(
                AllowsSecurityEvent(actual, "privilege-transition", result) ==
                    (expected.privilegeTransition && result == "success"),
                "Privilege policy mismatch");
            Expect(!AllowsSecurityEvent(actual, "namespace-change", result), "Deferred namespace event enabled");
            Expect(!AllowsSecurityEvent(actual, "time-change", result), "Unsupported time event enabled");
        }
    }
    ExpectRejected([] { AllowsSecurityEvent({}, "invalid", "success"); }, "Unknown policy family accepted");
    ExpectRejected([] { AllowsSecurityEvent({}, "network", "invalid"); }, "Unknown outcome accepted");
}

void TestProcessExecuted()
{
    auto event = ProcessEvent();
    for (const auto* name : c_processBooleanFields)
    {
        event[name] = false;
    }
    event["uid"] = 0U;
    event["argv"] = nlohmann::json::array({"echo", "", "quote\" newline\n backslash\\"});
    auto data = ProcessExecutedData(event, true);
    Expect(
        data.size() == 19 && data["schemaVersion"] == 4 && data["eventType"] == "ProcessExecuted" && data["outcome"] == "success",
        "Wrong exec schema");
    Expect(data["uid"]["value"] == 0U && data["uid"]["status"] == "present", "Root UID became missing");
    Expect(data["linuxEventNs"]["value"] == "18446744073709551615", "Nanoseconds lost precision");
    Expect(data["linuxEventNs"]["clock"] == "monotonic" && !data.contains("linuxStartNs"), "Non-PM start clock was emitted");
    Expect(data["mountNamespace"]["status"] == "unavailable", "Absent optional context was fabricated");
    event["namespacePid"] = 7U;
    event["mountNamespace"] = 42U;
    event["networkNamespace"] = 43U;
    event["cgroupId"] = UINT64_MAX;
    data = ProcessExecutedData(event, true);
    Expect(
        data["namespacePid"]["value"] == 7U && data["mountNamespace"]["value"] == 42U &&
            data["networkNamespace"]["value"] == 43U && data["cgroupId"]["value"] == "18446744073709551615",
        "Supported PM context was lost or narrowed");
    for (const auto* name : {"namespacePid", "mountNamespace", "networkNamespace"})
    {
        auto invalid = event;
        invalid[name] = uint64_t{UINT32_MAX} + 1;
        ExpectRejected([&] { ProcessExecutedData(invalid, false); }, "Oversized PM namespace context accepted");
    }
    auto invalidCgroup = event;
    invalidCgroup["cgroupId"] = -1;
    ExpectRejected([&] { ProcessExecutedData(invalidCgroup, false); }, "Negative PM cgroup accepted");
    Expect(data["argv"]["value"] == event["argv"], "Argument boundaries or empty argument lost");
    Expect(nlohmann::json::parse(data.dump()) == data, "Sample JSON did not round trip");
    event["argv"] = nlohmann::json::array();
    data = ProcessExecutedData(event, true);
    Expect(data["argv"]["status"] == "present" && data["argv"]["value"].empty(), "Empty argv became unavailable");
    event["cmdlineMissing"] = true;
    data = ProcessExecutedData(event, true);
    Expect(data["argv"]["value"].is_null() && data["argv"]["status"] == "unavailable", "Missing argv became empty");
    event["argv"] = "private-invalid-argv";
    data = ProcessExecutedData(event, false);
    Expect(data["argv"]["status"] == "disabled_by_policy" && data["argv"]["value"].is_null(), "Policy privacy failed");
    Expect(data.dump().find("private-") == std::string::npos, "Disabled argv was inspected or serialized");
    event.erase("argv");
    event["uidMissing"] = true;
    event["egid"] = UINT32_MAX;
    event["executableMissing"] = true;
    event["cwdTruncated"] = true;
    event["startTimeMissing"] = true;
    event["hidden"] = "private-extension";
    data = ProcessExecutedData(event, false);
    Expect(data["uid"]["value"].is_null() && data["egid"]["value"].is_null(), "Unavailable credentials were fabricated");
    Expect(data["gid"]["value"] == 1000U, "One missing credential hid an independent valid field");
    Expect(data["executable"]["value"].is_null() && data["cwd"]["truncated"] == true, "Path state lost");
    Expect(!data.contains("linuxStartNs") && !data.contains("hidden"), "Non-PM fields bypassed the whitelist");
    for (const auto* name : {"capabilities", "hash", "hostDeviceId", "wslDeviceId"})
    {
        Expect(!data.contains(name), "Unsupported PM request became a runtime placeholder");
    }
    event["cwdTruncated"] = 1;
    ExpectRejected([&] { ProcessExecutedData(event, false); }, "Malformed metadata accepted");
    event["cwdTruncated"] = false;
    event.erase("uidMissing");
    ExpectRejected([&] { ProcessExecutedData(event, false); }, "Missing metadata silently became present");
}

void TestProcessCreated()
{
    auto event = SecurityFixture("process-created");
    event["uid"] = 0U;
    event["argv"] = {"private-argument"};
    event["mountNamespace"] = 42U;
    event["networkNamespace"] = 43U;
    event["cgroupId"] = UINT64_MAX;
    auto data = ProcessCreatedData(event);
    Expect(data.size() == 16 && data["schemaVersion"] == 3 && data["eventType"] == "ProcessCreated", "Wrong creation schema");
    Expect(data["vmCreatorPid"]["value"] == 42U && data["namespacePid"]["value"] == 321U, "Creator or relative child PID lost");
    Expect(data["uid"]["status"] == "present" && data["uid"]["value"] == 0U, "Creation root UID became unavailable");
    Expect(
        data["mountNamespace"]["value"] == 42U && data["networkNamespace"]["value"] == 43U &&
            data["creatorCgroupId"]["value"] == "18446744073709551615",
        "Qualified creation namespace/cgroup context lost");
    for (const auto* name :
         {"creatorStartNs", "linuxStartNs", "creatorExecutable", "creatorCwd", "parentTid", "cloneFlags", "childCgroupId"})
    {
        Expect(!data.contains(name), "Non-PM or unsupported creation field was emitted");
    }
    Expect(data["linuxEventNs"]["value"] == "18446744073709551615", "Creation clock narrowed");
    Expect(!data.contains("argv") && data.dump().find("private-argument") == std::string::npos, "Creation leaked arguments");
    event["euid"] = UINT32_MAX;
    event["gid"] = nullptr;
    event["timestampNs"] = 0U;
    event["fields"]["parentPid"] = 0U;
    event["fields"]["executable"] = nullptr;
    event["fields"]["executableTruncated"] = true;
    event["fields"]["cwdTruncated"] = true;
    data = ProcessCreatedData(event);
    Expect(
        data["euid"]["value"].is_null() && data["gid"]["value"].is_null() && data["uid"]["value"] == 0U,
        "Independent creation credentials were conflated");
    Expect(
        data["linuxEventNs"]["value"].is_null() && data["vmCreatorPid"]["value"].is_null(),
        "Unknown creation IDs/clocks fabricated");
    Expect(!data.contains("creatorExecutable") && !data.contains("creatorCwd"), "Non-PM creation paths were emitted");
    event = SecurityFixture("process-created");
    event["fields"]["childPid"] = 234U;
    ExpectRejected([&] { ProcessCreatedData(event); }, "Creation child identity mismatch accepted");
    event = SecurityFixture("process-created");
    event["tid"] = 124U;
    event["fields"]["childTid"] = 124U;
    ExpectRejected([&] { ProcessCreatedData(event); }, "Thread creation presented as a process");
    event = SecurityFixture("process-created");
    event["pid"] = event["tid"] = event["fields"]["childPid"] = event["fields"]["childTid"] = 0U;
    ExpectRejected([&] { ProcessCreatedData(event); }, "Zero creation PID accepted");
    ExpectRejected([&] { ProcessCreatedData(SecurityFixture("connect")); }, "Network observation presented as creation");
}

void TestEnvelope()
{
    ExpectRejected([]() { ParseMessage(""); }, "Empty message accepted");
    ExpectRejected([]() { ParseMessage("{"); }, "Malformed JSON accepted");
    ExpectRejected([]() { ParseMessage("[]"); }, "Array envelope accepted");
    ExpectRejected([]() { ParseMessage(R"({"version":3,"kind":"ready"})"); }, "Unknown version accepted");
    ExpectRejected([]() { ParseMessage(R"({"version":"1","kind":"ready"})"); }, "String version accepted");
    ExpectRejected([]() { ParseMessage(R"({"version":1,"kind":false})"); }, "Nonstring kind accepted");
    ExpectRejected([]() { ParseMessage(std::string(c_maxMessageSize + 1, ' ')); }, "Oversized input accepted");

    const auto oversized = nlohmann::json{{"version", c_protocolVersion}, {"kind", std::string(c_maxMessageSize, 'x')}};
    ExpectRejected([&]() { EncodeMessage(oversized); }, "Oversized output accepted");
}

void TestProcessFields()
{
    auto event = ProcessEvent();
    ValidateProcessEvent(event);
    const auto encoded = EncodeMessage(event);
    const auto parsed = ParseMessage(encoded);
    ValidateProcessEvent(parsed);
    Expect(parsed["timestampNs"].get<uint64_t>() == std::numeric_limits<uint64_t>::max(), "64-bit timestamp lost precision");
    Expect(!parsed.contains("argv"), "Absent command line was fabricated");

    event["argv"] = nlohmann::json::array({"/usr/bin/echo", "", "quote\" newline\n backslash\\"});
    const auto arguments = EncodeMessage(event);
    Expect(arguments.find('\n') == arguments.size() - 1, "Escaped argument broke line framing");
    const auto restored = ParseMessage(arguments);
    ValidateProcessEvent(restored);
    Expect(restored["argv"][1] == "", "Empty argument was lost");
    Expect(restored["argv"] == event["argv"], "Argument escaping did not round-trip");

    auto invalid = event;
    invalid["pid"] = -1;
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Negative PID accepted");
    invalid = event;
    invalid["pidNamespace"] = uint64_t{UINT32_MAX} + 1;
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Overflowing namespace accepted");
    invalid = event;
    invalid["timestampNs"] = 1.5;
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Floating timestamp accepted");
    invalid = event;
    invalid.erase("cwd");
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Missing path accepted");
    invalid = event;
    invalid["executable"] = std::string(4097, 'x');
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Oversized path accepted");
    invalid = event;
    invalid["argv"] = "not-an-array";
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "String command line accepted");
    invalid = event;
    invalid["argv"] = nlohmann::json::array({7});
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Nonstring argument accepted");
    invalid = event;
    invalid["argv"] = nlohmann::json::array({std::string(4096, 'x')});
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Oversized argument data accepted");
    invalid = event;
    invalid["cmdlineTruncated"] = 0;
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Nonboolean truncation flag accepted");
    invalid = event;
    invalid["version"] = 1.0;
    ExpectRejected([&]() { ValidateProcessEvent(invalid); }, "Floating protocol version accepted");
}

} // namespace

int main()
{
    try
    {
        TestPolicies();
        TestEnvelope();
        TestProcessFields();
        TestProcessExecuted();
        TestProcessCreated();
        TestSecurityFields();
        std::cout << "Audit protocol: policy matrix, envelopes, bounds, argv and 64-bit timestamps passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Audit protocol test failed: " << error.what() << '\n';
        return 1;
    }
}
