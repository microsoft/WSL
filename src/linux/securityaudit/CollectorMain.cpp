// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Collector.h"

#include <charconv>
#include <cstdio>
#include <iostream>
#include <signal.h>

int wsl::audit::CollectorEntryPoint(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    try
    {
        if ((argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--container-id")) &&
            std::string_view(argv[1]) == "--inspect-pid")
        {
            const std::string_view text(argv[2]);
            uint32_t pid{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            {
                throw std::invalid_argument("--inspect-pid requires a positive Linux PID");
            }
            const std::string_view containerId = argc == 5 ? argv[4] : "";
            if (argc == 5 && containerId.empty())
            {
                throw std::invalid_argument("--container-id must not be empty");
            }
            std::cout << wsl::audit::InspectProcessNamespaces(pid, containerId).dump() << '\n';
            if (!std::cout)
            {
                throw std::runtime_error("Cannot write namespace inspection result");
            }
            return 0;
        }
        wsl::audit::CollectorOptions options;
        bool havePackage = false;
        bool haveDuration = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == "--package" && !havePackage && index + 1 < argc)
            {
                options.package = argv[++index];
                havePackage = true;
            }
            else if (argument == "--duration" && !haveDuration && index + 1 < argc)
            {
                const std::string_view text(argv[++index]);
                unsigned int seconds{};
                const auto result = std::from_chars(text.data(), text.data() + text.size(), seconds);
                if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || seconds == 0 || seconds > 86400)
                {
                    throw std::invalid_argument("--duration must be an integer from 1 to 86400 seconds");
                }
                options.duration = std::chrono::seconds(seconds);
                haveDuration = true;
            }
            else
            {
                throw std::invalid_argument("Usage: wsl_audit_collector [--package <LinuxPath>] [--duration <1..86400>]");
            }
        }
        if (options.package.empty())
        {
            throw std::invalid_argument("--package must not be empty");
        }
        return wsl::audit::RunCollector(options);
    }
    catch (const std::exception& error)
    {
        return wsl::audit::ReportCollectorFailure(error.what());
    }
}
