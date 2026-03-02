#pragma once

#include "WSLCExecutorHelpers.h"

namespace WSLCE2ETests {

#define WSLC_COMMAND(Name, Command) \
    template <typename... Args> \
    static WSLCExecutionResult Name(const Args&... args) \
    { \
        return ExecuteInternal(Command, args...); \
    }

struct WSLCCommand
{
    WSLC_COMMAND(Container, L"container")
    WSLC_COMMAND(ContainerCreate, L"container create")
    WSLC_COMMAND(ContainerList, L"container list")
    WSLC_COMMAND(ContainerDelete, L"container delete")

private:
    template <typename... Args>
    static WSLCExecutionResult ExecuteInternal(std::wstring commandLine, const Args&... args)
    {
        std::wstringstream ss;
        ss << commandLine;
        ((ss << L" " << args), ...);
        return WSLCExecutor::Execute(ss.str());
    }
};
} // namespace WSLCE2ETests