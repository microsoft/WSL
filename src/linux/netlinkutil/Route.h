// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <optional>
#include "address.h"

struct Route
{
    int family = -1;
    std::optional<Address> via;
    int dev = -1;
    bool defaultRoute = false;
    std::optional<Address> to;
    int metric = 0;
    bool isLoopbackRoute = false;

    Route(int family, const std::optional<Address>& via, int dev, bool defaultRoute, const std::optional<Address>& to, int metric);

    bool IsOnlink() const;
    bool IsMulticast() const;
};

std::ostream& operator<<(std::ostream& out, const Route& route);
