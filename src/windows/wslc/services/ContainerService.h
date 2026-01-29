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

struct StopContainerOptions
{
    int Signal = WSLASignalSIGTERM;
    ULONG Timeout = 5;
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLA_CONTAINER_STATE State;
};

class ContainerService
{
public:
    int Run(IWSLASession& session, std::string image, CreateOptions options);
    CreateContainerResult Create(IWSLASession& session, std::string image, CreateOptions options);
    void Start(IWSLASession& session, std::string id);
    void Stop(IWSLASession& session, std::string id, StopContainerOptions options);
    void Kill(IWSLASession& session, std::string id, int signal = WSLASignalSIGKILL);
    void Delete(IWSLASession& session, std::string id, bool force);
    std::vector<ContainerInformation> List(IWSLASession& session, std::vector<std::string> ids);
    void Exec();
    void Inspect();

private:
    void CreateInternal(
        IWSLASession& session,
        IWSLAContainer** container,
        std::vector<WSLA_PROCESS_FD>& fds,
        std::string image,
        CreateOptions options);
    void StartInternal(IWSLAContainer& container);
    void StopInternal(IWSLAContainer& container, const StopContainerOptions& options);
    std::vector<WSLA_PROCESS_FD> CreateFds(const CreateOptions& options);
};
}