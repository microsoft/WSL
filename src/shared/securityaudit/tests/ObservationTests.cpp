// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include "ObservationFixture.h"
#include <iostream>
#include <set>

using namespace wsl::audit;

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename F>
void Rejected(F&& callable)
{
    try
    {
        callable();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error("Malformed observation was accepted");
}

} // namespace

int main()
try
{
    std::set<uint32_t> ids;
    const std::set<std::string> pmFields{
        "schemaVersion",
        "eventType",
        "family",
        "operation",
        "outcome",
        "result",
        "vmPid",
        "vmTid",
        "pidNamespace",
        "mountNamespace",
        "networkNamespace",
        "cgroupId",
        "uid",
        "euid",
        "gid",
        "egid",
        "linuxEventNs",
        "requestedProtocol",
        "localIp",
        "localPort",
        "remoteIp",
        "remotePort",
        "requestedIp",
        "requestedPort",
        "dnsName",
        "oldUid",
        "oldGid",
        "newUid",
        "newGid",
        "namespaceTypes",
        "previousMountNamespace",
        "operationMountNamespace",
        "operationNetworkNamespace",
        "operationPidNamespace"};
    for (const auto& schema : c_observationSchemas)
    {
        Require(ids.insert(schema.eventId).second, "Event IDs must be unique");
        auto event = ObservationFixture(schema.operation);
        event["argv"] = {"private-argv"};
        event["private-extension"] = "private-value";
        const auto data = SecurityObservationData(event);
        Require(data.at("eventType") == schema.eventType && data.at("schemaVersion") == 3, "Schema identity differs");
        Require(
            data.size() == 5 + ObservationCommonFields(schema.operation).size() +
                               ObservationFields(SecurityFamily(schema.operation), schema.operation).size(),
            "Unexpected field set");
        Require(data.dump().find("private-") == std::string::npos, "Unknown fields leaked");
        Require(data.at("linuxEventNs").at("value") == "18446744073709551615", "Clock precision lost");
        Require(data.at("linuxEventNs").at("clock") == "monotonic", "Clock domain lost");
        for (const auto& [name, value] : data.items())
        {
            Require(pmFields.contains(name), "Non-PM field was emitted");
            Require(!value.is_object() || value.at("status") != "unsupported", "Unsupported PM request became a placeholder");
        }
        if (std::string_view(schema.operation) == "dns-query")
        {
            Require(!data.contains("euid") && !data.contains("cgroupId"), "Unsupported DNS context was emitted");
        }
        else
        {
            Require(data.at("euid").at("status") == "unavailable", "Ambiguous effective-ID zero was presented as root");
        }
        for (const auto& field : ObservationFields(SecurityFamily(schema.operation), schema.operation))
        {
            const auto state = ObservationFieldState(event, field.name);
            if (state != "available")
            {
                Require(
                    data.at(field.name).at("status").get<std::string_view>() == state && data.at(field.name).at("value").is_null(),
                    "Unsupported/inapplicable states lost");
            }
        }
        event["pid"] = 0U;
        Rejected([&] { SecurityObservationData(event); });
    }
    auto event = ObservationFixture("connect");
    event["uid"] = 0U;
    event["gid"] = 0U;
    event["euid"] = 1000U;
    event["egid"] = UINT32_MAX;
    auto data = SecurityObservationData(event);
    Require(
        data["uid"]["value"] == 0 && data["gid"]["value"] == 0 && data["euid"]["value"] == 1000,
        "Known zero IDs or valid nonzero effective ID discarded");
    Require(data["egid"]["status"] == "unavailable", "Credential sentinel became a real ID");
    for (const auto code : {INT64_MIN, int64_t{-115}, int64_t{0}, INT64_MAX})
    {
        event["result"] = code;
        event["outcome"] = code == -115 ? "pending" : code < 0 ? "failure" : "success";
        data = SecurityObservationData(event);
        Require(data["result"]["value"] == std::to_string(code), "Signed return value narrowed");
    }
    event["result"] = nullptr;
    event["outcome"] = "unknown";
    Require(SecurityObservationData(event)["result"]["status"] == "unavailable", "Unknown return defaulted to zero");
    event["fields"]["remotePort"] = 65536U;
    Rejected([&] { SecurityObservationData(event); });
    event = ObservationFixture("dns-query");
    event["result"] = -2;
    Require(SecurityObservationData(event)["outcome"] == "success", "DNS incorrectly used errno to infer outcome");
    event["fields"]["dnsAddresses"] = nlohmann::json::array();
    Require(!SecurityObservationData(event).contains("dnsAddresses"), "Non-PM DNS answer list was emitted");
    event["fields"]["dnsName"] = std::string("a\0b", 3);
    Rejected([&] { SecurityObservationData(event); });
    event = ObservationFixture("connect");
    event["fields"]["requestedProtocol"] = 6;
    Rejected([&] { SecurityObservationData(event); });
    event = ObservationFixture("setresgid");
    Require(!SecurityObservationData(event).contains("requestedSavedGid"), "Requested argument became an observed PM identity");
    event["fields"]["oldGid"] = 0U;
    Rejected([&] { SecurityObservationData(event); });
    Rejected([&] { GetObservationSchema("clock_settime"); });
    Rejected([&] { GetObservationSchema("process-created"); });
    std::cout << "13 observation schemas: identity, availability, sentinel, precision, privacy and validation passed.\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
