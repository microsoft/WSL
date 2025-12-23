#pragma once

#include <boost/asio.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace wsl::windows::service::wsla {

class DockerHTTPClient
{
    NON_COPYABLE(DockerHTTPClient);

public:
    using OnResponseBytes = std::function<void(gsl::span<char>)>;
    using OnImageProgress = std::function<void(const std::string&)>;

    DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE ExitingEvent, GUID VmId, ULONG ConnectTimeoutMs);


    uint32_t PullImage(const char* Name, const char* Tag, const OnImageProgress& Callback);
    std::pair<uint32_t, wil::unique_socket> SendRequest(boost::beast::http::verb Method, const std::string& Url, const std::string& Body = "", const OnResponseBytes& OnResponse = {});

private:
    wil::unique_socket ConnectSocket();

    ULONG m_connectTimeoutMs{};
    GUID m_vmId;
    shared::SocketChannel m_channel;
    HANDLE m_exitingEvent;
    wil::srwlock m_lock;
};
} // namespace wsl::windows::service::wsla