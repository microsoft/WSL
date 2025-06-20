// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "address.h"

struct InterfaceConfiguration
{
    std::vector<Address> Addresses;
    std::vector<Address> LocalAddresses;
    std::optional<Address> BroadcastAddress;
};
