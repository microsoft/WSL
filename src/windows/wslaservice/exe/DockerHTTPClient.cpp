#include "precomp.h"

#include "DockerHTTPClient.h"

using boost::beast::http::verb;
using wsl::windows::service::wsla::DockerHTTPClient;

DockerHTTPClient::DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE exitingEvent, GUID VmId, ULONG ConnectTimeoutMs) :
    m_exitingEvent(exitingEvent), m_channel(std::move(Channel)), m_vmId(VmId), m_connectTimeoutMs(ConnectTimeoutMs)
{
}

uint32_t DockerHTTPClient::PullImage(const char* Name, const char* Tag, const OnImageProgress& Callback)
{
    auto [code, _] = SendRequest(verb::post, std::format("http://localhost/images/create?fromImage=library/{}&tag={}", Name, Tag), {}, [Callback](const gsl::span<char>& span) {
        Callback(std::string{span.data(), span.size()});
    });

    return code;
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

std::pair<uint32_t, wil::unique_socket> DockerHTTPClient::SendRequest(boost::beast::http::verb Method, const std::string& Url, const std::string& Body, const OnResponseBytes& OnResponse)
{
    namespace http = boost::beast::http;

    boost::asio::io_context context;
    boost::asio::generic::stream_protocol::socket stream(context);

    // Write the request
    boost::asio::generic::stream_protocol hv_proto(AF_HYPERV, SOCK_STREAM);
    stream.assign(hv_proto, ConnectSocket().release());

    http::request<http::string_body> req{Method, Url, 11};
    req.set(http::field::host, "localhost");
    req.set(http::field::connection, "close");
    req.set(http::field::accept, "application/json");
    req.prepare_payload();

    http::write(stream, req);

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
            stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), buffer.size()), m_exitingEvent, MSG_PEEK);

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
        bytesRead = common::socket::Receive(stream.native_handle(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), i), m_exitingEvent);
        WI_ASSERT(bytesRead == i);

        boost::beast::error_code error;
        parser.put(boost::asio::buffer(buffer.data(), bytesRead), error);
        THROW_HR_IF(E_UNEXPECTED, error && error != boost::beast::http::error::need_more);
    }

    WSL_LOG("HTTPResult", TraceLoggingValue(Url.c_str(), "Url"), TraceLoggingValue(parser.get().result_int(), "Status"));

    if (OnResponse)
    {
        while (!parser.is_done())
        {
            boost::beast::flat_buffer adapter;

            parser.get().body().data = buffer.data();
            parser.get().body().size = buffer.size();
            http::read(stream, adapter, parser);


            WSL_LOG("Sizes", TraceLoggingValue(parser.get().body().size));

            auto bytesRead = buffer.size() - parser.get().body().size;

            OnResponse(gsl::span<char>{buffer.data(), bytesRead});
        }
    }

    return {parser.get().result_int(), wil::unique_socket{stream.release()}};
}
