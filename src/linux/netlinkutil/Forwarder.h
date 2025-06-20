// Copyright (C) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <thread>
#include <memory>

#include "Packet.h"

class IForwarder
{
public:
    virtual ~IForwarder() {};
};

template <typename ProcessingFunction, typename ExceptionHandler>
class Forwarder : public IForwarder
{
public:
    Forwarder(int SourceFd, int DestinationFd, ProcessingFunction Handler, ExceptionHandler exceptionHandler);
    ~Forwarder() override;

private:
    std::thread Worker;
    int SourceFd;
    int DestinationFd;
    int TerminateFd;
};

#include "Forwarder.hxx"
