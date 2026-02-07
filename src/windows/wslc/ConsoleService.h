#pragma once

#include <wslaservice.h>
#include "ConsoleModel.h"

namespace wslc::services
{
class ConsoleService
{
public:
    int AttachToCurrentConsole(wil::com_ptr<IWSLAProcess>&& process, wslc::models::ConsoleAttachOptions options);
    std::vector<WSLA_PROCESS_FD> BuildStdioDescriptors(bool tty, bool interactive);
};
}
