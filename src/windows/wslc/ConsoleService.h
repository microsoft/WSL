#pragma once

#include <wslaservice.h>
#include <WSLAContainerLauncher.h>

namespace wslc::services
{
class ConsoleService
{
public:
    int AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLAProcess&& process);
};
}
