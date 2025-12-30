#pragma once

#include <boost/asio.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include "docker_schema.h"

namespace wsl::windows::service::wsla {

class DockerHTTPException : public std::runtime_error
{
public:
    DockerHTTPException(uint16_t StatusCode, const std::string& Url, const std::string& RequestContent, const std::string& ResponseContent) :
        std::runtime_error(std::format("HTTP request failed: {} -> {} (Request: {}, Response: {})", Url, StatusCode, RequestContent, ResponseContent)),
        m_statusCode(StatusCode),
        m_url(Url),
        m_request(RequestContent),
        m_response(ResponseContent)
    {
    }

    template <typename T = docker_schema::ErrorResponse>
    T DockerMessage()
    {
        return wsl::shared::FromJson<T>(m_response.c_str());
    }

private:
    uint16_t m_statusCode{};
    std::string m_url;
    std::string m_request;
    std::string m_response;
};

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

    docker_schema::CreatedContainer CreateContainer(const docker_schema::CreateContainer& Request);
    void StartContainer(const std::string& Id);
    void StopContainer(const std::string& Id, int Signal, ULONG TimeoutSeconds);
    void DeleteContainer(const std::string& Id);

    wil::unique_socket AttachContainer(const std::string& Id);
    wil::unique_socket MonitorEvents();

    void ResizeContainerTty(const std::string& Id, ULONG Rows, ULONG Columns);

    uint32_t PullImage(const char* Name, const char* Tag, const OnImageProgress& Callback);

private:
    wil::unique_socket ConnectSocket();
    std::pair<uint32_t, std::string> SendRequest(
        boost::beast::http::verb Method, const std::string& Url, const std::string& Body = "");

    std::pair<uint32_t, wil::unique_socket> SendRequest(
        boost::beast::http::verb Method,
        const std::string& Url,
        const std::string& Body,
        const OnResponseBytes& OnResponse,
        const std::map<boost::beast::http::field, std::string>& Headers = {});

    template <typename TRequest = docker_schema::EmtpyRequest, typename TResponse = TRequest::TResponse>
    auto Transaction(boost::beast::http::verb Method, const std::string& Url, const TRequest& RequestObject = {})
    {
        std::string requestString;
        if constexpr (!std::is_same_v<TRequest, docker_schema::EmtpyRequest>)
        {
            requestString = wsl::shared::ToJson(RequestObject);
        }

        auto [statusCode, responseString] = SendRequest(Method, Url, requestString);

        if (statusCode < 200 || statusCode >= 300)
        {
            throw DockerHTTPException(statusCode, Url, requestString, responseString);
        }

        if constexpr (!std::is_same_v<TResponse, void>)
        {
            return wsl::shared::FromJson<TResponse>(responseString.c_str());
        }
    }

    ULONG m_connectTimeoutMs{};
    GUID m_vmId;
    shared::SocketChannel m_channel;
    HANDLE m_exitingEvent;
    wil::srwlock m_lock;
};
} // namespace wsl::windows::service::wsla