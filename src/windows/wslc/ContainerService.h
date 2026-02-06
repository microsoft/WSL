#pragma once
#include "SessionModel.h"
#include "ContainerModel.h"

namespace wslc::services
{
class ContainerService
{
public:
    int Run(wslc::models::Session& session, std::string image, wslc::models::ContainerCreateOptions options);
    wslc::models::CreateContainerResult Create(wslc::models::Session& session, std::string image, wslc::models::ContainerCreateOptions options);
    void Start(wslc::models::Session& session, std::string id);
    void Stop(wslc::models::Session& session, std::string id, wslc::models::StopContainerOptions options);
    void Kill(wslc::models::Session& session, std::string id, int signal = WSLASignalSIGKILL);
    void Delete(wslc::models::Session& session, std::string id, bool force);
    std::vector<wslc::models::ContainerInformation> List(wslc::models::Session& session);
    int Exec(wslc::models::Session& session, std::string id, wslc::models::ExecContainerOptions options);
    wsl::windows::common::docker_schema::InspectContainer Inspect(wslc::models::Session& session, std::string id);

private:
    void CreateInternal(
        wslc::models::Session& session,
        IWSLAContainer** container,
        std::vector<WSLA_PROCESS_FD>& fds,
        std::string image,
        const wslc::models::ContainerCreateOptions& options);
    void StartInternal(IWSLAContainer& container);
    void StopInternal(IWSLAContainer& container, const wslc::models::StopContainerOptions& options);
    void SetContainerOptions(
        WSLA_CONTAINER_OPTIONS& options,
        const std::string& name,
        bool tty,
        bool interactive,
        std::vector<WSLA_PROCESS_FD>& fds,
        const std::vector<std::string>& arguments,
        std::vector<const char*>& args);
};
}