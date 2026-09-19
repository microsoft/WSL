// Copyright (C) Microsoft Corporation. All rights reserved.

// libFuzzer harness for the WSLC SDK WinRT API.
//
// Exercises the WinRT projection of the session and container settings flow
// with fuzz-controlled inputs. Requires WSL service running on the host.

#include "precomp.h"

#include <winrt/Microsoft.WSL.Containers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>

#include "FuzzingHarness.h"

#include <cstdint>
#include <string>

using namespace winrt;
using namespace winrt::Microsoft::WSL::Containers;
using namespace winrt::Windows::Foundation;

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    winrt::init_apartment();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 10 || size > 4096)
    {
        return -1;
    }

    FuzzInput input{data, size};

    try
    {
        // Read session parameters from corpus
        std::wstring sessionName = input.ReadWideString();
        std::wstring storagePath = input.ReadWideString();

        // Build session settings via WinRT
        SessionSettings sessionSettings{hstring{sessionName}, hstring{storagePath}};
        sessionSettings.CpuCount(1);
        sessionSettings.MemoryMB(1024);
        sessionSettings.Timeout(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds{5000}});

        // Attempt to create session
        Session session{sessionSettings};
        session.Start();

        // Read container parameters from corpus
        std::string imageName = input.ReadString();
        ContainerSettings containerSettings{hstring{winrt::to_hstring(imageName)}};

        std::string containerName = input.ReadString();
        if (!containerName.empty())
        {
            containerSettings.Name(hstring{winrt::to_hstring(containerName)});
        }

        std::string hostName = input.ReadString();
        if (!hostName.empty())
        {
            containerSettings.HostName(hstring{winrt::to_hstring(hostName)});
        }

        // Fuzz port mappings
        uint8_t portCount = input.Read<uint8_t>();
        auto portMappings = containerSettings.PortMappings();
        for (uint8_t i = 0; i < portCount; ++i)
        {
            uint16_t windowsPort = input.Read<uint16_t>();
            uint16_t containerPort = input.Read<uint16_t>();
            auto protocol = static_cast<PortProtocol>(input.Read<uint8_t>() % 2);
            portMappings.Append(ContainerPortMapping{windowsPort, containerPort, protocol});
        }

        // Fuzz flags
        auto flags = static_cast<ContainerFlags>(input.Read<uint32_t>());
        containerSettings.Flags(flags);

        // Attempt container creation
        auto container = session.CreateContainer(containerSettings);

        // Cleanup
        session.Terminate();
    }
    catch (...)
    {
        // Expected — most fuzzed inputs will fail. ASAN crashes bypass this.
    }

    return 0;
}
