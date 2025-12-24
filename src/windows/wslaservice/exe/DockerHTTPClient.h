#pragma once

#include <boost/asio.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include "docker_schema.h"

namespace wsl::windows::service::wsla {

class DockerHTTPClient
{
    NON_COPYABLE(DockerHTTPClient);

public:
    using OnResponseBytes = std::function<void(gsl::span<char>)>;
    using OnImageProgress = std::function<void(const std::string&)>;

    template <typename T>
    struct RequestResult
    {
        uint32_t StatusCode;
        std::optional<T> ResponseObject;
        std::string ResponseString;
        std::string RequestString;

        std::string Format()
        {
            return std::format("{} -> {}({})", RequestString, StatusCode, ResponseString);
        }
    };

    template <>
    struct RequestResult<void>
    {
        uint32_t StatusCode;
        std::string ResponseString;
        std::string RequestString;

        std::string Format()
        {
            return std::format("{} -> {}({})", RequestString, StatusCode, ResponseString);
        }
    };

    DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE ExitingEvent, GUID VmId, ULONG ConnectTimeoutMs);

    RequestResult<docker_schema::CreatedContainer> CreateContainer(const docker_schema::CreateContainer& Request);
    RequestResult<void> StartContainer(const std::string& Id);

    wil::unique_socket AttachContainer(const std::string& Id);

    uint32_t PullImage(const char* Name, const char* Tag, const OnImageProgress& Callback);
    std::pair<uint32_t, wil::unique_socket> SendRequest(
        boost::beast::http::verb Method,
        const std::string& Url,
        const std::string& Body = "",
        const OnResponseBytes& OnResponse = {},
        const std::map<boost::beast::http::field, std::string>& Headers = {});

    std::pair<uint32_t, std::string> Transaction(
        boost::beast::http::verb Method, const std::string& Url, const std::string& Body = "");

private:
    wil::unique_socket ConnectSocket();

    template <typename TRequest>
    auto SendRequest(boost::beast::http::verb Method, const std::string& Url, const TRequest& Request)
    {
        RequestResult<typename TRequest::TResponse> result;
        result.RequestString = wsl::shared::ToJson(Request);
        std::tie(result.StatusCode, result.ResponseString) = Transaction(Method, Url, result.RequestString);

        if constexpr (!std::is_same_v<typename TRequest::TResponse, void>)
        {
            if (result.StatusCode >= 200 && result.StatusCode < 300)
            {
                result.ResponseObject = wsl::shared::FromJson<typename TRequest::TResponse>(result.ResponseString.c_str());
            }
        }

        return result;
    }

    ULONG m_connectTimeoutMs{};
    GUID m_vmId;
    shared::SocketChannel m_channel;
    HANDLE m_exitingEvent;
    wil::srwlock m_lock;
};
} // namespace wsl::windows::service::wsla