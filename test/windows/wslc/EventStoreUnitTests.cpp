// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "EventStore.h"

using namespace wsl::windows::service::wslc;

class EventStoreUnitTests
{
    WSLC_TEST_CLASS(EventStoreUnitTests)

    TEST_METHOD(DelayedEventsUseSessionPublicationOrder)
    {
        EventStore store;
        store.Record("container", "start", "restarted-container", {}, 2100000000LL);
        store.Record("container", "die", "restarted-container", {{"exitCode", "23"}}, 1900000000LL);
        store.Record("network", "connect", "other-network", {}, 1800000000LL);
        store.Record("volume", "create", "other-volume", {}, 2200000000LL);
        store.Record("container", "destroy", "restarted-container", {}, 2190000000LL);

        std::optional<uint64_t> sequence;
        const std::array<std::string, 5> actions{"start", "die", "connect", "create", "destroy"};
        const std::array<int64_t, 5> times{2100000000LL, 2100000000LL, 2100000000LL, 2200000000LL, 2200000000LL};
        for (size_t index = 0; index < actions.size(); ++index)
        {
            const auto event = store.Get(sequence, std::nullopt, std::nullopt, {}, nullptr);
            VERIFY_IS_TRUE(event.has_value());
            VERIFY_ARE_EQUAL(actions[index], event->Action);
            VERIFY_ARE_EQUAL(times[index], event->timeNano);
            if (event->Action == "die")
            {
                VERIFY_ARE_EQUAL(std::string{"23"}, event->Actor.Attributes.at("exitCode"));
            }
        }
    }

    TEST_METHOD(TimeWindowsUsePublicationTime)
    {
        EventStore store;
        store.Record("container", "create", "container", {}, 1900000000LL);
        store.Record("container", "start", "container", {}, 2100000000LL);
        store.Record("network", "connect", "network", {}, 1800000000LL);
        store.Record("volume", "create", "volume", {}, 3000000000LL);

        const auto since = std::chrono::sys_seconds{std::chrono::seconds{2}};
        const auto until = std::chrono::sys_seconds{std::chrono::seconds{3}};
        std::optional<uint64_t> sequence;
        auto event = store.Get(sequence, since, until, {}, nullptr);
        VERIFY_IS_TRUE(event.has_value());
        VERIFY_ARE_EQUAL(std::string{"start"}, event->Action);
        event = store.Get(sequence, since, until, {}, nullptr);
        VERIFY_IS_TRUE(event.has_value());
        VERIFY_ARE_EQUAL(std::string{"connect"}, event->Action);
        VERIFY_ARE_EQUAL(2100000000LL, event->timeNano);
        VERIFY_IS_FALSE(store.Get(sequence, since, until, {}, nullptr).has_value());

        sequence.reset();
        event = store.Get(sequence, since, until, {{"type", {"network"}}}, nullptr);
        VERIFY_IS_TRUE(event.has_value());
        VERIFY_ARE_EQUAL(std::string{"network"}, event->Actor.ID);
        VERIFY_IS_FALSE(store.Get(sequence, since, until, {{"type", {"network"}}}, nullptr).has_value());
    }
};
