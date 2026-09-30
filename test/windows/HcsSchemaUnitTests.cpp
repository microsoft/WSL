// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "hcs_schema.h"

namespace HcsSchemaUnitTests {
class HcsSchemaUnitTests
{
    WSL_TEST_CLASS(HcsSchemaUnitTests)

    // Verify that enabling automatic vNUMA emits the empty object HCS uses to derive the topology.
    TEST_METHOD(TopologyWithAutomaticNumaSerializesNumaObject)
    {
        wsl::windows::common::hcs::Topology topology{};
        topology.Numa.emplace();

        const auto json = nlohmann::json(topology);

        VERIFY_IS_TRUE(json.contains("Numa"));
        VERIFY_IS_TRUE(json.at("Numa").is_object());
        VERIFY_IS_TRUE(json.at("Numa").empty());
    }
};
} // namespace HcsSchemaUnitTests
