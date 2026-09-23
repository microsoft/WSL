/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCNetworkTests.cpp

Abstract:

    This file contains test cases for the WSLC network API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCNetworkTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCNetworkTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(NetworkCreateDeleteListTest)
    {
        const std::string networkName = "test-network";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        // The network must not exist yet. The predefined networks are always listed.
        VERIFY_IS_FALSE(NetworkIsListed(networkName));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        // Verify it appears in the list with correct fields.
        auto networks = ListNetworks();
        const auto created = std::ranges::find_if(networks, [&](const auto& network) { return network.Name == networkName; });
        VERIFY_ARE_NOT_EQUAL(networks.end(), created);
        VERIFY_ARE_EQUAL(std::string("bridge"), created->Driver);
        VERIFY_ARE_EQUAL(std::string("local"), created->Scope);
        VERIFY_IS_FALSE(created->Id.empty());
        VERIFY_IS_FALSE(created->Created.empty());

        // The label used to track wslc managed networks is an implementation detail and must not surface.
        VERIFY_IS_FALSE(created->Labels.contains("com.microsoft.wsl.network.managed"));

        // Duplicate name should fail.
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_defaultSession->CreateNetwork(&options, nullptr));

        cleanup.release();
        VERIFY_SUCCEEDED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        VERIFY_IS_FALSE(NetworkIsListed(networkName));

        // Delete non-existent should fail.
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->DeleteNetwork(networkName.c_str()));
    }

    std::vector<wsl::windows::common::wslc_schema::NetworkListEntry> ListNetworks(const std::vector<WSLCFilter>& Filters = {})
    {
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(Filters.empty() ? nullptr : Filters.data(), static_cast<ULONG>(Filters.size()), &output));

        return wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::NetworkListEntry>>(output.get());
    }

    bool NetworkIsListed(const std::string& Name)
    {
        const auto networks = ListNetworks();
        return std::ranges::any_of(networks, [&](const auto& network) { return network.Name == Name; });
    }

    void CreateNamedNetwork(const std::string& Name, const std::vector<WSLCLabel>& Labels = {})
    {
        WSLCNetworkOptions options{};
        options.Name = Name.c_str();
        options.Driver = "bridge";
        options.Labels = Labels.empty() ? nullptr : Labels.data();
        options.LabelsCount = static_cast<ULONG>(Labels.size());

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));
    }

    WSLC_TEST_METHOD(ListNetworksFilters)
    {
        const std::string netA = "wslc-flt-net-a";
        const std::string netB = "wslc-flt-net-b";
        const std::string netC = "wslc-flt-net-c";
        const std::string testLabelKey = "wslc.test.list_filter";
        const std::string testLabelValue = "1";
        const std::string testLabelKV = testLabelKey + "=" + testLabelValue;
        const std::string managedLabel = "com.microsoft.wsl.network.managed";

        auto cleanup = wil::scope_exit([&]() {
            for (const auto& name : {netA, netB, netC})
            {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(name.c_str()));
            }
        });

        CreateNamedNetwork(netA, {{testLabelKey.c_str(), testLabelValue.c_str()}, {"env", "prod"}, {"tier", "web"}});
        CreateNamedNetwork(netB, {{testLabelKey.c_str(), testLabelValue.c_str()}, {"env", "test"}});
        CreateNamedNetwork(netC, {{testLabelKey.c_str(), testLabelValue.c_str()}, {"env", "prod"}});

        auto expectListFails = [&](HRESULT expected, const std::vector<WSLCFilter>& filters) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_ansistring output;
            VERIFY_ARE_EQUAL(expected, m_defaultSession->ListNetworks(filtersPtr, filtersCount, &output));
        };

        auto expectList = [&](const std::vector<std::string>& expected,
                              const std::vector<WSLCFilter>& filters,
                              const std::source_location& source = std::source_location::current()) {
            std::vector<std::string> names;
            for (const auto& n : ListNetworks(filters))
            {
                names.emplace_back(n.Name);
                VERIFY_IS_FALSE(n.Id.empty());
                VERIFY_ARE_EQUAL(std::string("bridge"), n.Driver);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        const std::vector<std::string> all{netA, netB, netC};

        expectList(all, {{"label", testLabelKV.c_str()}});

        // label=<key>=<value> selects a subset within this test's scope.
        expectList({netA, netC}, {{"label", testLabelKV.c_str()}, {"label", "env=prod"}});
        expectList({netB}, {{"label", testLabelKV.c_str()}, {"label", "env=test"}});

        // Multiple label filters are AND'd.
        expectList({netA}, {{"label", testLabelKV.c_str()}, {"label", "env=prod"}, {"label", "tier=web"}});

        // label=<key> (key-only) matches any stored value.
        expectList(all, {{"label", testLabelKV.c_str()}, {"label", "env"}});

        // driver filter combined with the test-scope label.
        expectList(all, {{"label", testLabelKV.c_str()}, {"driver", "bridge"}});
        expectList({}, {{"label", testLabelKV.c_str()}, {"driver", "nonexistent"}});

        // Networks created by wslc carry the managed label, which can still be filtered on explicitly.
        expectList(all, {{"label", testLabelKV.c_str()}, {"label", managedLabel.c_str()}});

        // Predefined networks are not managed by wslc but are still listed.
        {
            const auto networks = ListNetworks();
            std::vector<std::string> names;
            for (const auto& n : networks)
            {
                names.emplace_back(n.Name);
            }

            for (const auto& predefined : {"bridge", "host", "none"})
            {
                VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, predefined));
            }
        }

        // Null filter key/value is rejected.
        expectListFails(E_POINTER, {{nullptr, "anything"}});
        expectListFails(E_POINTER, {{"label", nullptr}});
    }

    WSLC_TEST_METHOD(PruneNetworksTest)
    {
        auto expectPrune = [&](const std::vector<std::string>& expected,
                               const std::vector<WSLCFilter>& filters = {},
                               const std::source_location& source = std::source_location::current()) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_SUCCEEDED(m_defaultSession->PruneNetworks(filtersPtr, filtersCount, deleted.addressof(), deleted.size_address<ULONG>()));

            std::vector<std::string> names;
            for (const auto& n : deleted)
            {
                names.emplace_back(n);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        // Prune with no managed networks present returns empty.
        expectPrune({});

        // Prune removes unused managed networks.
        {
            const std::string a = "wslc-prune-net-a";
            const std::string b = "wslc-prune-net-b";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(a.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(b.c_str()));
            });

            CreateNamedNetwork(a);
            CreateNamedNetwork(b);

            expectPrune({a, b});

            VERIFY_IS_FALSE(NetworkIsListed(a));
            VERIFY_IS_FALSE(NetworkIsListed(b));

            cleanup.release();
        }

        // Label filter (key=value).
        {
            const std::string labeled = "wslc-prune-net-labeled";
            const std::string unlabeled = "wslc-prune-net-unlabeled";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(unlabeled.c_str()));
            });

            CreateNamedNetwork(labeled, {{"wslc-prune-net-test", "yes"}});
            CreateNamedNetwork(unlabeled);

            expectPrune({labeled}, {{"label", "wslc-prune-net-test=yes"}});

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(unlabeled.c_str()));
            cleanup.release();
        }

        // Label filter (negation).
        {
            const std::string keep = "wslc-prune-net-keep";
            const std::string drop = "wslc-prune-net-drop";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(keep.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(drop.c_str()));
            });

            CreateNamedNetwork(keep, {{"wslc-prune-net-keep", "yes"}});
            CreateNamedNetwork(drop);

            expectPrune({drop}, {{"label!", "wslc-prune-net-keep"}});

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(keep.c_str()));
            cleanup.release();
        }

        // Filter with null Key rejected.
        {
            WSLCFilter filters[] = {{nullptr, "true"}};

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_ARE_EQUAL(
                E_POINTER, m_defaultSession->PruneNetworks(filters, ARRAYSIZE(filters), deleted.addressof(), deleted.size_address<ULONG>()));
        }

        // Filter with null Value rejected.
        {
            WSLCFilter filters[] = {{"label", nullptr}};

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_ARE_EQUAL(
                E_POINTER, m_defaultSession->PruneNetworks(filters, ARRAYSIZE(filters), deleted.addressof(), deleted.size_address<ULONG>()));
        }
    }

    WSLC_TEST_METHOD(NetworkCreateWithSubnetTest)
    {
        const std::string networkName = "subnet-test-net";
        const std::string subnet = "172.28.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
    }

    WSLC_TEST_METHOD(NetworkCreateInternalTest)
    {
        const std::string networkName = "internal-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Internal = TRUE;

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(networkName, inspect.Name);
        VERIFY_IS_TRUE(inspect.Internal);
    }

    WSLC_TEST_METHOD(NetworkCreateWithLabelsTest)
    {
        const std::string networkName = "labels-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCLabel labels[] = {
            {.Key = "com.example.env", .Value = "test"},
            {.Key = "com.example.team", .Value = "infra"},
        };

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        options.Labels = labels;
        options.LabelsCount = ARRAYSIZE(labels);

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        const auto networks = ListNetworks();
        VERIFY_IS_TRUE(std::ranges::any_of(networks, [&](const auto& network) { return network.Name == networkName; }));
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidDriverAndOptionTest)
    {
        const std::string networkName = "bad-network-create-input";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";

        auto verifyInvalid = [&](PCWSTR expectedMessage) {
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
            ValidateCOMErrorMessageContains(expectedMessage);
        };

        // Invalid drivers (unknown, wrong case, empty)
        for (const char* driver : {"overlay", "Bridge", ""})
        {
            options.Driver = driver;
            verifyInvalid(L"Unsupported network driver:");
        }

        // Gateway specified without Subnet
        {
            options.Driver = "bridge";
            options.Subnet = nullptr;
            options.Gateway = "172.44.0.1";
            verifyInvalid(L"--gateway");
        }

        // IpRange specified without Subnet
        {
            options.Driver = "bridge";
            options.Subnet = nullptr;
            options.Gateway = nullptr;
            options.IpRange = "172.44.10.0/24";
            verifyInvalid(L"--ip-range");
        }
    }

    WSLC_TEST_METHOD(NetworkCreateDefaultDriverTest)
    {
        const std::string networkName = "default-driver-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = nullptr;
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        const auto networks = ListNetworks();
        const auto created = std::ranges::find_if(networks, [&](const auto& network) { return network.Name == networkName; });
        VERIFY_ARE_NOT_EQUAL(networks.end(), created);
        VERIFY_ARE_EQUAL(std::string("bridge"), created->Driver);
    }

    WSLC_TEST_METHOD(NetworkCreateReservedNameTest)
    {
        WSLCNetworkOptions options{};
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        options.Name = "bridge";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"bridge");

        options.Name = "host";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"host");

        options.Name = "none";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"none");
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidNameTest)
    {
        WSLCNetworkOptions options{};
        options.Name = "invalid name!";
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid name!");
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidSubnetTest)
    {
        const std::string networkName = "bad-subnet-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = "not-a-cidr";

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid subnet");

        wil::unique_cotaskmem_ansistring output;
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->InspectNetwork(networkName.c_str(), &output));
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidGatewayTest)
    {
        const std::string networkName = "bad-gateway-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = "172.27.0.0/16";
        options.Gateway = "999.999.999.999";

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid gateway");

        wil::unique_cotaskmem_ansistring output;
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->InspectNetwork(networkName.c_str(), &output));
    }

    WSLC_TEST_METHOD(NetworkCreateWithGatewayTest)
    {
        const std::string networkName = "gateway-test-net";
        const std::string subnet = "172.31.0.0/16";
        const std::string gateway = "172.31.0.1";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        options.Gateway = gateway.c_str();

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
        VERIFY_ARE_EQUAL(gateway, inspect.IPAM.Config->at(0).Gateway);
    }

    WSLC_TEST_METHOD(NetworkCreateWithIpRangeTest)
    {
        const std::string networkName = "ip-range-test-net";
        const std::string subnet = "172.32.0.0/16";
        const std::string ipRange = "172.32.10.0/24";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        options.IpRange = ipRange.c_str();

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
        VERIFY_ARE_EQUAL(ipRange, inspect.IPAM.Config->at(0).IPRange);
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidIpRangeTest)
    {
        const std::string networkName = "bad-ip-range-net";
        const std::string subnet = "172.33.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        options.IpRange = "10.0.0.0/24";

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid ip-range");

        wil::unique_cotaskmem_ansistring output;
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->InspectNetwork(networkName.c_str(), &output));
    }

    WSLC_TEST_METHOD(NetworkSessionRecoveryWithIpRangeTest)
    {
        const std::string networkName = "recovery-ip-range-net";
        const std::string subnet = "172.34.0.0/16";
        const std::string ipRange = "172.34.20.0/24";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        options.IpRange = ipRange.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        // Reset the session (simulates session restart).
        ResetTestSession();

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
        VERIFY_ARE_EQUAL(ipRange, inspect.IPAM.Config->at(0).IPRange);
    }

    WSLC_TEST_METHOD(NetworkCreateWithArbitraryDriverOptsTest)
    {
        const std::string networkName = "arbitrary-opts-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCDriverOption opts[] = {{"my.abc.key", "mygod"}, {"com.example.flag", "1"}};

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = opts;
        options.DriverOptsCount = ARRAYSIZE(opts);

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.Options.contains("my.abc.key"));
        VERIFY_IS_TRUE(inspect.Options.contains("com.example.flag"));
        VERIFY_ARE_EQUAL(std::string("mygod"), inspect.Options.at("my.abc.key"));
        VERIFY_ARE_EQUAL(std::string("1"), inspect.Options.at("com.example.flag"));
    }

    WSLC_TEST_METHOD(NetworkSessionRecoveryTest)
    {
        const std::string networkName = "recovery-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCDriverOption recoveryOpts[] = {{"recovery.test.key", "preserved"}};

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = recoveryOpts;
        options.DriverOptsCount = ARRAYSIZE(recoveryOpts);
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        // Reset the session (simulates session restart).
        ResetTestSession();

        const auto networks = ListNetworks();
        const auto recovered = std::ranges::find_if(networks, [&](const auto& network) { return network.Name == networkName; });
        VERIFY_ARE_NOT_EQUAL(networks.end(), recovered);
        VERIFY_ARE_EQUAL(std::string("bridge"), recovered->Driver);
        VERIFY_IS_FALSE(recovered->Id.empty());

        // Verify arbitrary driver options survive session recovery.
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.Options.contains("recovery.test.key"));
        VERIFY_ARE_EQUAL(std::string("preserved"), inspect.Options.at("recovery.test.key"));
    }

    WSLC_TEST_METHOD(NetworkMultipleCreateListDeleteTest)
    {
        const std::string networkNameA = "net-a";
        const std::string networkNameB = "net-b";
        const std::string networkNameC = "net-c";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameA.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameC.c_str()));

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameA.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameC.c_str()));
        });

        WSLCNetworkOptions optionsA{};
        optionsA.Name = networkNameA.c_str();
        optionsA.Driver = "bridge";
        optionsA.DriverOpts = nullptr;
        optionsA.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsA, nullptr));

        WSLCNetworkOptions optionsB{};
        optionsB.Name = networkNameB.c_str();
        optionsB.Driver = "bridge";
        optionsB.Subnet = "172.29.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsB, nullptr));

        WSLCNetworkOptions optionsC{};
        optionsC.Name = networkNameC.c_str();
        optionsC.Driver = "bridge";
        optionsC.Internal = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsC, nullptr));

        auto listedNames = [&]() {
            std::vector<std::string> names;
            for (const auto& network : ListNetworks())
            {
                names.push_back(network.Name);
            }

            return names;
        };

        auto names = listedNames();
        VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, networkNameA));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, networkNameB));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, networkNameC));

        VERIFY_SUCCEEDED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));

        names = listedNames();
        VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, networkNameA));
        VERIFY_ARE_EQUAL(names.end(), std::ranges::find(names, networkNameB));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::ranges::find(names, networkNameC));
    }

    WSLC_TEST_METHOD(NetworkInspectTest)
    {
        const std::string networkName = "test-inspect-network";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(inspect.Name, networkName);
        VERIFY_ARE_EQUAL(inspect.Driver, std::string("bridge"));
        VERIFY_IS_FALSE(inspect.Id.empty());
        VERIFY_IS_FALSE(inspect.Internal);
    }

    WSLC_TEST_METHOD(NetworkInspectWithSubnetTest)
    {
        const std::string networkName = "test-inspect-subnet-net";
        const std::string subnet = "172.30.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(inspect.Name, networkName);
        VERIFY_ARE_EQUAL(inspect.Driver, std::string("bridge"));
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
    }

    WSLC_TEST_METHOD(NetworkInspectNotFoundTest)
    {
        wil::unique_cotaskmem_ansistring output;
        auto hr = m_defaultSession->InspectNetwork("nonexistent-network", &output);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, hr);
        ValidateCOMErrorMessageContains(L"nonexistent-network");
    }
};
