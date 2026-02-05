#pragma once

#include <string>
#include <vector>

namespace wslc::services
{
struct SessionInformation
{
    ULONG SessionId;
    DWORD CreatorPid;
    std::wstring DisplayName;
};

class ShellService
{
public:
    int Attach(std::wstring name);
    std::vector<SessionInformation> List();
};
}