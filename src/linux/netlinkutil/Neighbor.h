// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "address.h"

struct Neighbor
{
    Address ipAddress;
    MacAddress macAddress;
    int dev = -1;

    Neighbor(const Address& ipAddress, const MacAddress& macAddress, int dev);

    int getFamily() const;
};
