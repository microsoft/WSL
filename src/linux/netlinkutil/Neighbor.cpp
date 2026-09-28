// Copyright (C) Microsoft Corporation. All rights reserved.
#include "Neighbor.h"

Neighbor::Neighbor(const Address& ipAddress, const MacAddress& macAddress, int dev) :
    ipAddress(ipAddress), macAddress(macAddress), dev(dev)
{
}

int Neighbor::getFamily() const
{
    return ipAddress.Family();
}
