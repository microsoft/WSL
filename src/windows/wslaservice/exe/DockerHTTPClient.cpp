#include "precomp.h"

#include "DockerHTTPClient.h"

using wsl::windows::service::wsla::DockerHTTPClient;

DockerHTTPClient::DockerHTTPClient(wsl::shared::SocketChannel&& Channel, HANDLE exitingEvent, GUID VmId, ULONG ConnectTimeoutMs) :
    m_exitingEvent(exitingEvent), m_channel(std::move(Channel)), m_vmId(VmId), m_connectTimeoutMs(ConnectTimeoutMs)
{
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

void DockerHTTPClient::SendRequest(boost::beast::http::verb Method, const std::string& Url, const OnResponseBytes& OnResponse, const std::string& Body)
{
    namespace http = boost::beast::http;

    boost::asio::io_context context;
    boost::asio::generic::stream_protocol::socket stream(context);

    // Write the request
    {
        boost::asio::generic::stream_protocol hv_proto(AF_HYPERV, SOCK_STREAM);
        stream.assign(hv_proto, ConnectSocket().release());

        // boost::beast::basic_stream<boost::asio::generic::stream_protocol::socket> wrapped_stream;

        http::request<http::string_body> req{http::verb::get, Url, 11};
        req.set(http::field::host, "hvsocket"); // label only; AF_HYPERV doesn't do DNS
        req.prepare_payload();

        http::write(stream, req);
    }

    wil::unique_socket socket{stream.release()};

    // Parse the response header
    std::vector<char> buffer(16 * 4096);
    http::response_parser<http::buffer_body> parser;
    parser.eager(false);
    parser.skip(false);

    size_t lineFeeds = 0;

    // Consume the socket until the header is reached
    while (!parser.is_header_done())
    {
        // Peek for the end of the HTTP header '\r\n'
        auto bytesRead = common::socket::Receive(
            socket.get(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), buffer.size()), m_exitingEvent, MSG_PEEK);

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
        bytesRead = common::socket::Receive(socket.get(), gsl::span(reinterpret_cast<gsl::byte*>(buffer.data()), i), m_exitingEvent);
        WI_ASSERT(bytesRead == i);

        boost::beast::error_code error;
        parser.put(boost::asio::buffer(buffer.data(), bytesRead), error);
        THROW_HR_IF(E_UNEXPECTED, error && error != boost::beast::http::error::need_more);
    }

    WSL_LOG("HTTPResult", TraceLoggingValue(parser.get().result_int(), "Status"));

    boost::asio::generic::stream_protocol hv_proto(AF_HYPERV, SOCK_STREAM);
    stream.assign(hv_proto, socket.release());

    while (!parser.is_done())
    {
        boost::beast::flat_buffer adapter;

        parser.get().body().data = buffer.data();
        parser.get().body().size = buffer.size();
        http::read(stream, adapter, parser);

        auto bytesRead = parser.get().body().size - buffer.size();

        OnResponse(gsl::span<char>{buffer.data(), bytesRead});
    }
}
