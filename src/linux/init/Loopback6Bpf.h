// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <optional>

// The loaded loopback6 relay tc/BPF programs. The caller owns the file descriptors and must close them
// after attaching them (e.g. via Interface::BpfAttachTcClassifier).
struct Loopback6BpfPrograms
{
    int ingressFd = -1;
    int egressFd = -1;
};

// Loads the loopback6 relay ingress/egress tc/BPF programs and their shared map from the data embedded in
// loopback6_relay.skel.h. Returns std::nullopt (logging the reason) if the programs were not compiled into
// this image or the kernel rejected them, so callers can treat BPF support as best-effort.
std::optional<Loopback6BpfPrograms> LoadLoopback6RelayPrograms();
