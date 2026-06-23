// Copyright (C) Microsoft Corporation. All rights reserved.

// libFuzzer harness for the WSLC SDK C API.
//
// Exercises the session and container settings flow with fuzz-controlled inputs.
// Requires WSL service running on the host (installed via setup.ps1 on OneFuzz VMs).

#include "precomp.h"

#include "wslcsdk.h"

#include "FuzzingHarness.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 10 || size > 4096)
    {
        return -1;
    }

    FuzzInput input{data, size};

    // Read session parameters from corpus
    std::wstring sessionName = input.ReadWideString();
    std::wstring storagePath = input.ReadWideString();

    // Initialize session settings
    WslcSessionSettings sessionSettings{};
    HRESULT hr = WslcInitSessionSettings(sessionName.c_str(), storagePath.c_str(), &sessionSettings);
    if (FAILED(hr))
    {
        return -1;
    }

    // Configure session with minimal resources
    WslcSetSessionSettingsCpuCount(&sessionSettings, 1);
    WslcSetSessionSettingsMemory(&sessionSettings, 512);
    WslcSetSessionSettingsTimeout(&sessionSettings, 5000);

    // Read container parameters
    std::string imageName = input.ReadString();
    std::string containerName = input.ReadString();
    std::string hostName = input.ReadString();

    // Read port mappings
    uint8_t portCount = input.Read<uint8_t>();
    std::vector<WslcContainerPortMapping> ports(portCount);
    for (uint8_t i = 0; i < portCount; ++i)
    {
        ports[i].windowsPort = input.Read<uint16_t>();
        ports[i].containerPort = input.Read<uint16_t>();
        ports[i].protocol = static_cast<WslcPortProtocol>(input.Read<uint8_t>() % 2);
    }

    // Read flags
    auto containerFlags = static_cast<WslcContainerFlags>(input.Read<uint32_t>());

    // Attempt to create session — may fail if service isn't available
    WslcSession session = nullptr;
    hr = WslcCreateSession(&sessionSettings, &session, nullptr);
    if (FAILED(hr) || !session)
    {
        return 0;
    }

    // Initialize container settings
    WslcContainerSettings containerSettings{};
    hr = WslcInitContainerSettings(imageName.c_str(), &containerSettings);
    if (FAILED(hr))
    {
        WslcTerminateSession(session);
        WslcReleaseSession(session);
        return 0;
    }

    if (!containerName.empty())
    {
        WslcSetContainerSettingsName(&containerSettings, containerName.c_str());
    }
    if (!hostName.empty())
    {
        WslcSetContainerSettingsHostName(&containerSettings, hostName.c_str());
    }
    if (portCount > 0)
    {
        WslcSetContainerSettingsPortMappings(&containerSettings, ports.data(), portCount);
    }
    WslcSetContainerSettingsFlags(&containerSettings, containerFlags);

    // Attempt container creation — exercises validation and service communication
    WslcContainer container = nullptr;
    hr = WslcCreateContainer(session, &containerSettings, &container, nullptr);
    if (SUCCEEDED(hr) && container)
    {
        WslcReleaseContainer(container);
    }

    WslcTerminateSession(session);
    WslcReleaseSession(session);

    return 0;
}
