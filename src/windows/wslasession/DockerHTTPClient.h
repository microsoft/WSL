/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerHTTPClient.h

Abstract:

    This file contains the definition of the Docker HTTP client.

--*/

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include "relay.hpp"
#include "docker_schema.h"

#define THROW_DOCKER_USER_ERROR_MSG(_Ex, _Msg, ...) \
    if ((_Ex).StatusCode() >= 400 && (_Ex).StatusCode() <= 500) \
    { \
        THROW_HR_WITH_USER_ERROR_MSG(E_FAIL, (_Ex).DockerMessage<wsl::windows::common::docker_schema::ErrorResponse>().message, _Msg, __VA_ARGS__); \
    } \
    else \
    { \
        THROW_HR_MSG(E_FAIL, _Msg ". Error: %hs", __VA_ARGS__, (_Ex).what()); \
    }

#define CATCH_AND_THROW_DOCKER_USER_ERROR(_Msg, ...) \
    catch (const DockerHTTPException& e) \
    { \
        THROW_DOCKER_USER_ERROR_MSG(e, _Msg, __VA_ARGS__) \
    }

namespace wsl::windows::service::wsla {

class DockerHTTPException : public std::runtime_error
{
public:
    DockerHTTPException(uint16_t StatusCode, boost::beast::http::verb Method, const std::string& Url, const std::string& RequestContent, const std::string& ResponseContent) :
        std::runtime_error(std::format(
            "HTTP request failed: {} {} -> {} (Request: {}, Response: {})", boost::beast::http::to_string(Method), Url, StatusCode, RequestContent, ResponseContent)),
        m_statusCode(StatusCode),
        m_url(Url),
        m_request(RequestContent),
        m_response(ResponseContent)
    {
    }

    template <typename T = docker_schema::ErrorResponse>
    T DockerMessage() const
    {
        return wsl::shared::FromJson<T>(m_response.c_str());
    }

    uint16_t StatusCode() const noexcept
    {
        return m_statusCode;
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

    struct HTTPRequestContext
    {
        NON_COPYABLE(HTTPRequestContext);
        NON_MOVABLE(HTTPRequestContext);

        HTTPRequestContext(wil::unique_socket&& Socket) : stream(context)
        {
            boost::asio::generic::stream_protocol hv_proto(AF_HYPERV, SOCK_STREAM);
            stream.assign(hv_proto, Socket.release());
        }

        boost::asio::io_context context;
        boost::asio::generic::stream_protocol::socket stream;
    };

    DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE ExitingEvent, GUID VmId, ULONG ConnectTimeoutMs);

    // Container management.
    std::vector<common::docker_schema::ContainerInfo> ListContainers(bool all = false);
    common::docker_schema::CreatedContainer CreateContainer(const common::docker_schema::CreateContainer& Request, const std::optional<std::string>& Name);
    void StartContainer(const std::string& Id);
    void StopContainer(const std::string& Id, std::optional<WSLASignal> Signal, std::optional<ULONG> TimeoutSeconds);
    void DeleteContainer(const std::string& Id);
    void SignalContainer(const std::string& Id, int Signal);
    std::string InspectContainer(const std::string& Id);
    std::string InspectExec(const std::string& Id);
    wil::unique_socket AttachContainer(const std::string& Id);
    void ResizeContainerTty(const std::string& Id, ULONG Rows, ULONG Columns);
    wil::unique_socket ContainerLogs(const std::string& Id, WSLALogsFlags Flags, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail);
    std::pair<uint32_t, wil::unique_socket> ExportContainer(const std::string& ContainerID);

    // Image management.
    std::unique_ptr<HTTPRequestContext> PullImage(const std::string& Repo, const std::optional<std::string>& Tag);
    std::unique_ptr<HTTPRequestContext> ImportImage(const std::string& Repo, const std::string& Tag, uint64_t ContentLength);
    std::unique_ptr<HTTPRequestContext> LoadImage(uint64_t ContentLength);
    void TagImage(const std::string& Id, const std::string& Repo, const std::string& Tag);
    std::vector<common::docker_schema::Image> ListImages();
    std::vector<common::docker_schema::DeletedImage> DeleteImage(const char* Image, bool Force, bool NoPrune); // Image can be ID or Repo:Tag.
    std::pair<uint32_t, wil::unique_socket> SaveImage(const std::string& NameOrId);

    // Exec.
    common::docker_schema::CreateExecResponse CreateExec(const std::string& Container, const common::docker_schema::CreateExec& Request);
    wil::unique_socket StartExec(const std::string& Id, const common::docker_schema::StartExec& Request);
    void ResizeExecTty(const std::string& Id, ULONG Rows, ULONG Columns);

    wil::unique_socket MonitorEvents();

    struct DockerHttpResponseHandle : public common::relay::ReadHandle
    {
        NON_COPYABLE(DockerHttpResponseHandle);
        NON_MOVABLE(DockerHttpResponseHandle);

        DockerHttpResponseHandle(
            HTTPRequestContext& context,
            std::function<void(const boost::beast::http::message<false, boost::beast::http::buffer_body>&)>&& OnResponseHeader,
            std::function<void(const gsl::span<char>&)>&& OnResponseBytes,
            std::function<void()>&& OnCompleted = []() {});

        ~DockerHttpResponseHandle();

    private:
        void OnRead(const gsl::span<char>& Content);
        void OnResponseBytes(const gsl::span<char>& Content);

        HTTPRequestContext& Context;
        std::function<void(const boost::beast::http::message<false, boost::beast::http::buffer_body>&)> OnResponseHeader;
        std::function<void(const gsl::span<char>&)> OnResponse;
        std::function<void()> OnCompleted;
        boost::beast::http::response_parser<boost::beast::http::buffer_body> Parser;
        size_t LineFeeds = 0;
        std::optional<size_t> RemainingContentLength;
        std::optional<common::relay::HTTPChunkBasedReadHandle> ResponseParser;
    };

private:
    class URL
    {
    public:
        std::string Get() const;
        void SetParameter(std::string&& Key, std::string&& Value);
        void SetParameter(std::string&& Key, const std::string& Value);
        void SetParameter(std::string&& Key, const char* Value); // Overload so that pointers don't resolve to the bool method.
        void SetParameter(std::string&& Key, bool Value);

        template <typename... Args>
        static auto Create(std::format_string<decltype(URL::Escape(std::declval<Args>()))...> Url, Args&&... args)
        {
            WI_ASSERT(Url.get().find_first_of("?!") == std::string::npos);

            return URL(std::format(Url, Escape(std::forward<Args>(args))...));
        }

    private:
        URL(std::string&& Path);

        static std::string Escape(const std::string& Value);

        std::string m_path;
        std::map<std::string, std::string> m_parameters;
    };

    wil::unique_socket ConnectSocket();

    std::unique_ptr<HTTPRequestContext> SendRequestImpl(
        boost::beast::http::verb Method, const URL& Url, const std::string& Body, const std::map<boost::beast::http::field, std::string>& Headers);

    std::pair<uint32_t, std::string> SendRequestAndReadResponse(
        boost::beast::http::verb Method, const URL& Url, const std::string& Body = "");

    std::pair<uint32_t, wil::unique_socket> SendRequest(
        boost::beast::http::verb Method, const URL& Url, const std::string& Body, const std::map<boost::beast::http::field, std::string>& Headers = {});

    template <typename TRequest = common::docker_schema::EmptyRequest, typename TResponse = TRequest::TResponse>
    auto Transaction(boost::beast::http::verb Method, const URL& Url, const TRequest& RequestObject = {})
    {
        std::string requestString;
        if constexpr (!std::is_same_v<TRequest, common::docker_schema::EmptyRequest>)
        {
            requestString = wsl::shared::ToJson(RequestObject);
        }

        auto [statusCode, responseString] = SendRequestAndReadResponse(Method, Url, requestString);

        if (statusCode < 200 || statusCode >= 300)
        {
            throw DockerHTTPException(statusCode, Method, Url.Get(), requestString, responseString);
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