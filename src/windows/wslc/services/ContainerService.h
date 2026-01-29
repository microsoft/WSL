#pragma once
#include <wslservice.h>

namespace wslc::services
{
struct RunOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
};

class ContainerService
{
public:
    int Run(IWSLASession& session, std::string image, RunOptions options);
    void Create();
    void Start();
    void Stop();
    void Kill();
    void Delete();
    void List();
    void Exec();
    void Inspect();
};
}