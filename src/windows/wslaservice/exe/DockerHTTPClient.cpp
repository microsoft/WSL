#include "precomp.h"

#include "DockerHTTPClient.h"

namespace http = boost::beast::http;
using boost::beast::http::verb;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::service::wsla::DockerHTTPClient;
using namespace wsl::windows::common;

DockerHTTPClient::DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE exitingEvent, GUID VmId, ULONG ConnectTimeoutMs) :
    m_exitingEvent(exitingEvent), m_channel(std::move(Channel)), m_vmId(VmId), m_connectTimeoutMs(ConnectTimeoutMs)
{
}

uint32_t DockerHTTPClient::PullImage(const char* Name, const char* Tag, const OnImageProgress& Callback)
{
    auto [code, _] = SendRequest(
        verb::post,
        std::format("http://localhost/images/create?fromImage=library/{}&tag={}", Name, Tag),
        {},
        [Callback](const gsl::span<char>& span) { Callback(std::string{span.data(), span.size()}); });

    return code;
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::LoadImage(const std::string& Repo, const std::string& Tag, uint64_t ContentLength)
{
    return SendRequestImpl(
        verb::post,
        std::format("http://localhost/images/load"),
        {},
        {{http::field::content_type, "application/x-tar"}, {http::field::content_length, std::to_string(ContentLength)}});
}

std::unique_ptr<DockerHTTPClient::HTTPRequestContext> DockerHTTPClient::ImportImage(const std::string& Repo, const std::string& Tag, uint64_t ContentLength)
{
    return SendRequestImpl(
        verb::post,
        std::format("http://localhost/images/create?fromSrc=-&repo={}&tag={}", Repo, Tag),
        {},
        {{http::field::content_type, "application/x-tar"}, {http::field::content_length, std::to_string(ContentLength)}});
}

void DockerHTTPClient::TagImage(const std::string& Id, const std::string& Repo, const std::string& Tag)
{
    Transaction<docker_schema::EmtpyRequest>(verb::post, std::format("http://localhost/images/{}/tag?repo={}&tag={}", Id, Repo, Tag));
}

std::vector<docker_schema::Image> DockerHTTPClient::ListImages()
{
    return Transaction<docker_schema::EmtpyRequest, std::vector<docker_schema::Image>>(verb::get, "http://localhost/images/json");
}

docker_schema::CreatedContainer DockerHTTPClient::CreateContainer(const docker_schema::CreateContainer& Request)
{
    // TODO: Url escaping.
    return Transaction<docker_schema::CreateContainer>(verb::post, "http://localhost/containers/create", Request);
}

void DockerHTTPClient::ResizeContainerTty(const std::string& Id, ULONG Rows, ULONG Columns)
{
    Transaction(verb::post, std::format("http://localhost/containers/{}/resize?w={}&h={}", Id, Columns, Rows));
}

void DockerHTTPClient::StartContainer(const std::string& Id)
{
    Transaction(verb::post, std::format("http://localhost/containers/{}/start", Id));
}

void DockerHTTPClient::StopContainer(const std::string& Id, int Signal, ULONG TimeoutSeconds)
{
    Transaction(verb::post, std::format("http://localhost/containers/{}/stop?signal={}&t={}", Id, Signal, TimeoutSeconds));
}

void DockerHTTPClient::SignalContainer(const std::string& Id, int Signal)
{
    Transaction(verb::post, std::format("http://localhost/containers/{}/kill?signal={}", Id, Signal));
}

void DockerHTTPClient::DeleteContainer(const std::string& Id)
{
    Transaction(verb::delete_, std::format("http://localhost/containers/{}", Id));
}

std::string DockerHTTPClient::InspectContainer(const std::string& Id)
{
    auto url = std::format("http://localhost/containers/{}/json", Id);
    auto [code, response] = SendRequest(verb::get, url);

    if (code < 200 || code >= 300)
    {
        throw DockerHTTPException(code, verb::get, url, "", response);
    }

    return response;
}

wil::unique_socket DockerHTTPClient::AttachContainer(const std::string& Id)
{
    std::map<boost::beast::http::field, std::string> headers{
        {boost::beast::http::field::upgrade, "tcp"}, {boost::beast::http::field::connection, "upgrade"}};

    auto url = std::format("http://localhost/containers/{}/attach?stream=1&stdin=1&stdout=1&stderr=1&logs=true", Id);
    auto [status, socket] = SendRequest(verb::post, url, {}, {}, headers);

    if (status != 101)
    {
        throw DockerHTTPException(status, verb::post, url, "", "");
    }

    return std::move(socket);
}

wil::unique_socket DockerHTTPClient::MonitorEvents()
{
    auto url = "http://localhost/events";
    auto [status, socket] = SendRequest(verb::get, url, {}, {});

    if (status != 200)
    {
        throw DockerHTTPException(status, verb::get, url, "", "");
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

std::pair<uint32_t, std::string> DockerHTTPClient::SendRequest(verb Method, const std::string& Url, const std::string& Body)
{
    std::string responseBody;
    auto OnResponse = [&responseBody](const gsl::span<char>& span) { responseBody.append(span.data(), span.size()); };

    auto [status, _] = SendRequest(Method, Url, Body, OnResponse);

    return {status, std::move(responseBody)};
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

        // Feed the parser up to the end of the heaer.
        boost::beast::error_code error;
        Parser.put(boost::asio::buffer(Content.data(), i), error);

        THROW_HR_IF_MSG(
            E_UNEXPECTED, error && error != boost::beast::http::error::need_more, "Error parsing HTTP response: %hs", error.what().c_str());

        if (Parser.is_header_done())
        {
            const auto& response = Parser.get();
            OnResponseHeader(response);

            // If the response is chunked, then create a chunked reader.
            // TODO: Proper header parsing.
            auto transferEncoding = response.find(http::field::transfer_encoding);
            if (transferEncoding != response.end() && transferEncoding->value() == "chunked")
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
    verb Method, const std::string& Url, const std::string& Body, const std::map<boost::beast::http::field, std::string>& Headers)
{
    auto context = std::make_unique<DockerHTTPClient::HTTPRequestContext>(ConnectSocket());

    http::request<http::string_body> req{Method, Url, 11};
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

    WSL_LOG("HTTPRequestDebug", TraceLoggingValue(Url.c_str(), "Url"), TraceLoggingValue(requestString.c_str(), "Request"));

#endif

    return std::move(context);
}

std::pair<uint32_t, wil::unique_socket> DockerHTTPClient::SendRequest(
    verb Method,
    const std::string& Url,
    const std::string& Body,
    const OnResponseBytes& OnResponse,
    const std::map<boost::beast::http::field, std::string>& Headers)
{
    // Write the request
    auto context = SendRequestImpl(Method, Url, Body, Headers);

    // Parse the response header
    std::vector<char> buffer(16 * 4096);
    http::response_parser<http::buffer_body> parser;
    parser.eager(false);
    parser.skip(false);

    size_t lineFeeds = 0;

    // Consume the socket until the header end is reached
    while (!parser.is_header_done())
    {
        // Peek for the end of the HTTP header '\r\n'
        auto bytesRead = common::socket::Receive(
            context->stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), buffer.size()), m_exitingEvent, MSG_PEEK);

        size_t i{};
        for (i = 0; i < bytesRead && lineFeeds < 2; i++)
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

        // Consumme the buffer from the socket.
        bytesRead = common::socket::Receive(
            context->stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), i), m_exitingEvent);
        WI_ASSERT(bytesRead == i);

        boost::beast::error_code error;
        parser.put(boost::asio::buffer(buffer.data(), bytesRead), error);
        THROW_HR_IF_MSG(
            E_UNEXPECTED, error && error != boost::beast::http::error::need_more, "Error parsing HTTP response: %hs", error.what().c_str());
    }

    WSL_LOG("HTTPResult", TraceLoggingValue(Url.c_str(), "Url"), TraceLoggingValue(parser.get().result_int(), "Status"));

    if (OnResponse)
    {
        while (!parser.is_done())
        {
            boost::beast::flat_buffer adapter;

            parser.get().body().data = buffer.data();
            parser.get().body().size = buffer.size();
            http::read(context->stream, adapter, parser);

            auto bytesRead = buffer.size() - parser.get().body().size;

            OnResponse(gsl::span<char>{buffer.data(), bytesRead});
        }
    }

    return {parser.get().result_int(), wil::unique_socket{context->stream.release()}};
}
