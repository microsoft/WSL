#pragma once
#include <wslservice.h>

namespace wslc::services
{
struct CreateOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
};

struct CreateContainerResult
{
    std::string Id;
};

class ContainerService
{
public:
    int Run(IWSLASession& session, std::string image, CreateOptions options);
    CreateContainerResult Create(IWSLASession& session, std::string image, CreateOptions options);
    void Start();
    void Stop();
    void Kill();
    void Delete();
    void List();
    void Exec();
    void Inspect();

private:
    void CreateInternal(
        IWSLASession& session,
        IWSLAContainer** container,
        std::vector<WSLA_PROCESS_FD>& fds,
        std::string image,
        CreateOptions options);
    std::vector<WSLA_PROCESS_FD> CreateFds(const CreateOptions& options);
};
}