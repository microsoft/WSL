/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerHTTPClient.cpp

Abstract:

    This file contains the implementation of the Docker HTTP client.
    This class is designed to wrap calls to the docker API over a socket channel.

    The flow of an HTTP request is:

    - Create a new hvsocket channel by sending a WSLA_FORK message to init.
    - Connect the new socket to the docker unix socket server via WSLA_UNIX_CONNECT
    - Once connected, send the HTTP request over that socket.

    Some HTTP requests have simple response bodies that can be read right away, and some others upgrade
    the connection to TCP (like attaching to a process stdio, importing a tar, ...). For those,
    we return the socket without reading the response body, so the caller can interact directly with the stream.

--*/

#include "precomp.h"

#include <winrt/Windows.Foundation.h>
#include "DockerHTTPClient.h"

namespace http = boost::beast::http;
using boost::beast::http::verb;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::service::wsla::DockerHTTPClient;
using namespace wsl::windows::common;

namespace {

bool IsResponseChunked(const http::response_parser<http::buffer_body>::value_type& response)
{
    auto transferEncoding = response.find(http::field::transfer_encoding);
    if (transferEncoding == response.end())
    {
        return false;
    }

    if (transferEncoding->value() != "chunked")
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unknown transfer encoding: %hs", std::string(transferEncoding->value()).c_str());
    }

    return true;
}
} // namespace

DockerHTTPClient::URL::URL(std::string&& Path) : m_path(std::move(Path))
{
}

void DockerHTTPClient::URL::SetParameter(std::string&& Key, std::string&& Value)
{
    m_parameters.emplace(std::move(Key), std::move(Value));
}

void DockerHTTPClient::URL::SetParameter(std::string&& Key, const std::string& Value)
{
    m_parameters.emplace(std::move(Key), Value);
}

void DockerHTTPClient::URL::SetParameter(std::string&& Key, const char* Value)
{
    SetParameter(std::move(Key), std::string(Value));
}

void DockerHTTPClient::URL::SetParameter(std::string&& Key, bool Value)
{
    m_parameters.emplace(std::move(Key), Value ? "true" : "false");
}

std::string DockerHTTPClient::URL::Get() const
{
    constexpr auto urlPrefix = "http://localhost";

    std::stringstream url;
    url << urlPrefix;
    url << m_path;

    if (!m_parameters.empty())
    {
        url << "?";
        bool first = true;
        for (const auto& [key, value] : m_parameters)
        {
            if (!first)
            {
                url << "&";
            }

            url << key << "=" << Escape(value);
            first = false;
        }
    }

    return url.str();
}

std::string DockerHTTPClient::URL::Escape(const std::string& Value)
{
    auto escaped = winrt::Windows::Foundation::Uri::EscapeComponent(winrt::to_hstring(Value));

    return wsl::shared::string::WideToMultiByte(escaped.c_str());
}

DockerHTTPClient::DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE exitingEvent, GUID VmId, ULONG ConnectTimeoutMs) :
    m_exitingEvent(exitingEvent), m_channel(std::move(Channel)), m_vmId(VmId), m_connectTimeoutMs(ConnectTimeoutMs)
{
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::PullImage(const std::string& Repo, const std::optional<std::string>& Tag)
{
    auto url = URL::Create("/images/create");
    url.SetParameter("fromImage", std::format("library/{}", Repo));

    if (Tag.has_value())
    {
        url.SetParameter("tag", Tag.value());
    }

    return SendRequestImpl(verb::post, url, {}, {});
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::LoadImage(uint64_t ContentLength)
{
    return SendRequestImpl(
        verb::post,
        URL::Create("/images/load"),
        {},
        {{http::field::content_type, "application/x-tar"}, {http::field::content_length, std::to_string(ContentLength)}});
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::ImportImage(const std::string& Repo, const std::string& Tag, uint64_t ContentLength)
{
    auto url = URL::Create("/images/create");
    url.SetParameter("tag", Tag);
    url.SetParameter("repo", Repo);
    url.SetParameter("fromSrc", "-");

    return SendRequestImpl(
        verb::post, url, {}, {{http::field::content_type, "application/x-tar"}, {http::field::content_length, std::to_string(ContentLength)}});
}

void DockerHTTPClient::TagImage(const std::string& Id, const std::string& Repo, const std::string& Tag)
{
    auto url = URL::Create("/images/{}", Id);
    url.SetParameter("repo", Repo);
    url.SetParameter("tag", Tag);

    Transaction<docker_schema::EmptyRequest>(verb::post, url);
}

std::vector<docker_schema::Image> DockerHTTPClient::ListImages()
{
    return Transaction<docker_schema::EmptyRequest, std::vector<docker_schema::Image>>(verb::get, URL::Create("/images/json"));
}

std::vector<docker_schema::DeletedImage> wsl::windows::service::wsla::DockerHTTPClient::DeleteImage(const char* Image, bool Force, bool NoPrune)
{
    auto url = URL::Create("/images/{}", Image);
    url.SetParameter("force", Force);
    url.SetParameter("noprune", NoPrune);

    return Transaction<docker_schema::EmptyRequest, std::vector<docker_schema::DeletedImage>>(verb::delete_, url);
}

std::pair<uint32_t, wil::unique_socket> DockerHTTPClient::SaveImage(const std::string& NameOrId)
{
    return SendRequest(verb::get, URL::Create("/images/{}/get", NameOrId), {}, {});
}

std::vector<docker_schema::ContainerInfo> DockerHTTPClient::ListContainers(bool all)
{
    auto url = URL::Create("/containers/json");
    url.SetParameter("all", all);

    return Transaction<docker_schema::EmptyRequest, std::vector<docker_schema::ContainerInfo>>(verb::get, url);
}

docker_schema::CreatedContainer DockerHTTPClient::CreateContainer(const docker_schema::CreateContainer& Request, const std::optional<std::string>& Name)
{
    auto url = URL::Create("/containers/create");
    if (Name.has_value())
    {
        url.SetParameter("name", Name.value());
    }

    return Transaction<docker_schema::CreateContainer>(verb::post, url, Request);
}

void DockerHTTPClient::ResizeContainerTty(const std::string& Id, ULONG Rows, ULONG Columns)
{
    auto url = URL::Create("/containers/{}/resize", Id);
    url.SetParameter("w", std::to_string(Columns));
    url.SetParameter("h", std::to_string(Rows));

    Transaction(verb::post, url);
}

void DockerHTTPClient::StartContainer(const std::string& Id)
{
    Transaction(verb::post, URL::Create("/containers/{}/start", Id));
}

void DockerHTTPClient::StopContainer(const std::string& Id, std::optional<WSLASignal> Signal, std::optional<ULONG> TimeoutSeconds)
{
    auto url = URL::Create("/containers/{}/stop", Id);
    if (Signal.has_value())
    {
        url.SetParameter("signal", std::to_string(static_cast<int>(Signal.value())));
    }

    if (TimeoutSeconds.has_value())
    {
        url.SetParameter("t", std::to_string(TimeoutSeconds.value()));
    }

    Transaction(verb::post, url);
}

void DockerHTTPClient::SignalContainer(const std::string& Id, int Signal)
{
    auto url = URL::Create("/containers/{}/kill", Id);
    url.SetParameter("signal", std::to_string(Signal));

    Transaction(verb::post, url);
}

void DockerHTTPClient::DeleteContainer(const std::string& Id)
{
    Transaction(verb::delete_, URL::Create("/containers/{}", Id));
}

std::string DockerHTTPClient::InspectContainer(const std::string& Id)
{
    auto url = URL::Create("/containers/{}/json", Id);
    auto [code, response] = SendRequestAndReadResponse(verb::get, url);

    if (code < 200 || code >= 300)
    {
        throw DockerHTTPException(code, verb::get, url.Get(), "", response);
    }

    return response;
}

wil::unique_socket DockerHTTPClient::AttachContainer(const std::string& Id)
{
    std::map<boost::beast::http::field, std::string> headers{
        {boost::beast::http::field::upgrade, "tcp"}, {boost::beast::http::field::connection, "upgrade"}};

    auto url = URL::Create("/containers/{}/attach", Id);
    url.SetParameter("stream", true);
    url.SetParameter("stdin", true);
    url.SetParameter("stdout", true);
    url.SetParameter("stderr", true);

    auto [status, socket] = SendRequest(verb::post, url, {}, headers);

    if (status != 101)
    {
        throw DockerHTTPException(status, verb::post, url.Get(), "", "");
    }

    return std::move(socket);
}

std::pair<uint32_t, wil::unique_socket> DockerHTTPClient::ExportContainer(const std::string& ContainerNameOrId)
{
    return SendRequest(verb::get, URL::Create("/containers/{}/export", ContainerNameOrId), {}, {});
}

wil::unique_socket DockerHTTPClient::ContainerLogs(const std::string& Id, WSLALogsFlags Flags, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
{
    auto url = URL::Create("/containers/{}/logs", Id);
    url.SetParameter("follow", WI_IsFlagSet(Flags, WSLALogsFlagsFollow));
    url.SetParameter("stdout", true);
    url.SetParameter("stderr", true);
    url.SetParameter("timestamps", WI_IsFlagSet(Flags, WSLALogsFlagsTimestamps));

    if (Tail != 0)
    {
        url.SetParameter("tail", std::to_string(Tail));
    }

    if (Until != 0)
    {
        url.SetParameter("until", std::to_string(Until));
    }

    if (Since != 0)
    {
        url.SetParameter("since", std::to_string(Since));
    }

    auto [status, socket] = SendRequest(verb::get, url, {}, {});
    if (status != 200)
    {
        throw DockerHTTPException(status, verb::get, url.Get(), "", "");
    }

    return std::move(socket);
}

docker_schema::CreateExecResponse DockerHTTPClient::CreateExec(const std::string& Container, const docker_schema::CreateExec& Request)
{
    return Transaction<docker_schema::CreateExec>(verb::post, URL::Create("/containers/{}/exec", Container), Request);
}

wil::unique_socket DockerHTTPClient::StartExec(const std::string& Id, const common::docker_schema::StartExec& Request)
{
    std::map<boost::beast::http::field, std::string> headers{
        {boost::beast::http::field::upgrade, "tcp"}, {boost::beast::http::field::connection, "upgrade"}};

    auto url = URL::Create("/exec/{}/start", Id);

    auto body = wsl::shared::ToJson(Request);
    auto [status, socket] = SendRequest(verb::post, url, body, headers);
    if (status != 101)
    {
        throw DockerHTTPException(status, verb::post, url.Get(), body, "");
    }
    return std::move(socket);
}

void DockerHTTPClient::ResizeExecTty(const std::string& Id, ULONG Rows, ULONG Columns)
{
    auto url = URL::Create("/exec/{}/resize", Id);
    url.SetParameter("w", std::to_string(Columns));
    url.SetParameter("h", std::to_string(Rows));

    Transaction(verb::post, url);
}

wil::unique_socket DockerHTTPClient::MonitorEvents()
{
    auto url = URL::Create("/events");
    auto [status, socket] = SendRequest(verb::get, url, {});

    if (status != 200)
    {
        throw DockerHTTPException(status, verb::get, url.Get(), "", "");
    }

    return std::move(socket);
}

wil::unique_socket DockerHTTPClient::ConnectSocket()
{
    auto lock = m_lock.lock_exclusive();

    // Send a fork message.
    WSLA_FORK message;
    message.ForkType = WSLA_FORK::Thread;
    const auto& response = m_channel.Transaction(message);

    THROW_HR_IF_MSG(E_FAIL, response.Pid <= 0, "fork() returned %i", response.Pid);

    // Connect the new hvsocket.
    wsl::shared::SocketChannel newChannel{
        wsl::windows::common::hvsocket::Connect(m_vmId, response.Port, m_exitingEvent, m_connectTimeoutMs), "DockerClient", m_exitingEvent};
    lock.reset();

    // Connect that socket to the docker unix socket.
    shared::MessageWriter<WSLA_UNIX_CONNECT> writer;
    writer.WriteString(writer->PathOffset, "/var/run/docker.sock");

    auto result = newChannel.Transaction<WSLA_UNIX_CONNECT>(writer.Span());
    THROW_HR_IF_MSG(E_FAIL, result.Result < 0, "Failed to connect to unix socket: '/var/run/docker.sock', %i", result.Result);

    return newChannel.Release();
}

std::pair<uint32_t, std::string> DockerHTTPClient::SendRequestAndReadResponse(verb Method, const URL& Url, const std::string& Body)
{
    // Send the request.
    auto context = SendRequestImpl(Method, Url, Body, {});

    // Read the response header and body.
    std::optional<boost::beast::http::status> status;
    std::string responseBody;
    auto OnResponse = [&responseBody](const gsl::span<char>& span) { responseBody.append(span.data(), span.size()); };

    auto onHttpResponse = [&](const auto& response) { status = response.result(); };
    MultiHandleWait io;

    io.AddHandle(std::make_unique<relay::EventHandle>(m_exitingEvent, [&]() { THROW_HR(E_ABORT); }));
    io.AddHandle(std::make_unique<DockerHttpResponseHandle>(*context, std::move(onHttpResponse), std::move(OnResponse)), MultiHandleWait::CancelOnCompleted);

    io.Run({});

    return {static_cast<uint32_t>(status.value()), responseBody};
}

DockerHTTPClient::DockerHttpResponseHandle::DockerHttpResponseHandle(
    HTTPRequestContext& context,
    std::function<void(const http::message<false, http::buffer_body>&)>&& onResponseHeader,
    std::function<void(const gsl::span<char>&)>&& onResponseBytes,
    std::function<void()>&& onCompleted) :
    common::relay::ReadHandle(
        HandleWrapper{context.stream.native_handle()}, std::bind(&DockerHttpResponseHandle::OnRead, this, std::placeholders::_1)),
    Context(context),
    OnResponseHeader(std::move(onResponseHeader)),
    OnResponse(std::move(onResponseBytes)),
    OnCompleted(std::move(onCompleted))
{
}

DockerHTTPClient::DockerHttpResponseHandle::~DockerHttpResponseHandle()
{
    if (State == common::relay::IOHandleStatus::Completed)
    {
        OnCompleted();
    }
}

void DockerHTTPClient::DockerHttpResponseHandle::OnRead(const gsl::span<char>& Content)
{
    // If the HTTP parser is done, then these bytes are part of the response body
    if (Parser.is_header_done())
    {
        OnResponseBytes(Content);
    }
    else
    {
        // Otherwise keep parsing the HTTP response header.
        size_t i{};
        for (i = 0; i < Content.size() && LineFeeds < 2; i++)
        {
            if (Content[i] == '\n')
            {
                LineFeeds++;
            }
            else if (Content[i] != '\r')
            {
                LineFeeds = 0;
            }
        }

        // Feed the parser up to the end of the header.
        boost::beast::error_code error;
        Parser.put(boost::asio::buffer(Content.data(), i), error);

        THROW_HR_IF_MSG(
            E_UNEXPECTED, error && error != boost::beast::http::error::need_more, "Error parsing HTTP response: %hs", error.what().c_str());

        if (Parser.is_header_done())
        {
            const auto& response = Parser.get();
            OnResponseHeader(response);

            // If the response is chunked, then create a chunked reader.
            if (IsResponseChunked(response))
            {
                ResponseParser.emplace(HandleWrapper{Context.stream.native_handle()}, std::move(OnResponse));
            }

            auto contentLength = response.find(http::field::content_length);
            if (contentLength != response.end())
            {
                RemainingContentLength = std::stoul(contentLength->value());
            }
        }

        // If any buffer remains, then it's part of the response body.
        auto remaining = Content.subspan(i);
        if (!remaining.empty())
        {
            WI_ASSERT(Parser.is_header_done());
            OnResponseBytes(remaining);
        }
    }
}

void DockerHTTPClient::DockerHttpResponseHandle::OnResponseBytes(const gsl::span<char>& Content)
{
    auto span = Content;

    // If the HTTP response had a Content-Length, make sure not to read past it.
    if (RemainingContentLength.has_value())
    {
        auto consume = std::min(span.size(), RemainingContentLength.value());

        *RemainingContentLength -= consume;
        if (*RemainingContentLength == 0)
        {
            State = common::relay::IOHandleStatus::Completed;
        }

        span = span.subspan(0, consume);
    }

    if (ResponseParser.has_value())
    {
        ResponseParser->OnRead(span);
    }
    else
    {
        OnResponse(span);
    }
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::SendRequestImpl(
    verb Method, const URL& Url, const std::string& Body, const std::map<boost::beast::http::field, std::string>& Headers)
{
    auto context = std::make_unique<DockerHTTPClient::HTTPRequestContext>(ConnectSocket());

    http::request<http::string_body> req{Method, Url.Get(), 11};
    if (!Body.empty())
    {
        req.set(http::field::content_type, "application/json");
        req.body() = Body;

        // N.B. prepare_payload() overrides content-length.
        req.prepare_payload();
    }

    req.set(http::field::host, "localhost");
    req.set(http::field::connection, "close");
    req.set(http::field::accept, "application/json");

    for (const auto [field, value] : Headers)
    {
        req.set(field, value);
    }

    http::write(context->stream, req);

#ifdef WSLA_HTTP_DEBUG

    std::ostringstream oss;
    oss << req;

    auto requestString = oss.str();

    WSL_LOG("HTTPRequestDebug", TraceLoggingValue(Url.Get().c_str(), "Url"), TraceLoggingValue(requestString.c_str(), "Request"));

#endif

    return std::move(context);
}

std::pair<uint32_t, wil::unique_socket> DockerHTTPClient::SendRequest(
    verb Method, const URL& Url, const std::string& Body, const std::map<boost::beast::http::field, std::string>& Headers)
{
    // Write the request
    auto context = SendRequestImpl(Method, Url, Body, Headers);

    // Parse the response header
    constexpr auto bufferSize = 16 * 1024;
    size_t Offset = 0;
    std::vector<char> buffer;
    http::response_parser<http::buffer_body> parser;
    parser.eager(false);
    parser.skip(false);

    size_t lineFeeds = 0;
    // Consume the socket until the header end is reached
    while (!parser.is_header_done())
    {
        buffer.resize(Offset + bufferSize);

        // Peek for the end of the HTTP header '\r\n'
        auto bytesRead = common::socket::Receive(
            context->stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data() + Offset), bufferSize), m_exitingEvent, MSG_PEEK);

        THROW_HR_IF(E_ABORT, bytesRead == 0);

        size_t i{};
        for (i = 0; i < bytesRead + Offset && lineFeeds < 2; i++)
        {
            if (buffer[i] == '\n')
            {
                lineFeeds++;
            }
            else if (buffer[i] != '\r')
            {
                lineFeeds = 0;
            }
        }

        // Consume the buffer from the socket.
        bytesRead = common::socket::Receive(
            context->stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data() + Offset), i - Offset), m_exitingEvent);
        WI_ASSERT(bytesRead == i - Offset);

        Offset += bytesRead;
        buffer.resize(Offset);

        if (lineFeeds == 2) // Header is complete, feed it to the parser.
        {

#ifdef WSLA_HTTP_DEBUG

            buffer.push_back('\0');
            WSL_LOG(
                "HTTPResponseDebug", TraceLoggingValue(Url.Get().c_str(), "Url"), TraceLoggingValue(buffer.data(), "Response"));
            buffer.pop_back();

#endif

            boost::beast::error_code error;
            parser.put(boost::asio::buffer(buffer.data(), buffer.size()), error);

            THROW_HR_IF_MSG(
                E_UNEXPECTED,
                error && error != boost::beast::http::error::need_more,
                "Error parsing HTTP response: %hs",
                error.what().c_str());
        }
    }

    return {parser.get().result_int(), wil::unique_socket{context->stream.release()}};
}
