// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "UsbIpTestServer.hpp"
#include <bit>
#include <deque>
#include <usb.h>

namespace wsl::test::usbip {
namespace {

    constexpr uint16_t c_version = 0x0111;
    constexpr uint16_t c_list = 0x0005;
    constexpr uint16_t c_import = 0x0003;
    constexpr uint16_t c_request = 0x8000;
    constexpr uint32_t c_submit = 1, c_unlink = 2, c_submitReply = 3, c_unlinkReply = 4;
    constexpr int c_stall = -32, c_cancelled = -104, c_noSpace = -28;
    constexpr size_t c_maxTransfer = 64 * 1024;

    struct Operation
    {
        uint16_t Version, Code;
        uint32_t Status;
    };

    struct Export
    {
        char Path[256], BusId[32];
        uint32_t Bus, Number, Speed;
        uint16_t Vendor, Product, Release;
        uint8_t Class, Subclass, Protocol, Configuration, Configurations, Interfaces;
    };

    struct Setup
    {
        uint8_t Type, Request;
        uint16_t Value, Index, Length;
    };

    struct Transfer
    {
        uint32_t Command, Sequence, DeviceId, Direction, Endpoint;
        uint32_t StatusOrFlags, Length, StartFrame, Packets, Interval;
        Setup Control;
    };

    static_assert(sizeof(Operation) == 8 && sizeof(Export) == 312 && sizeof(Transfer) == 48 && sizeof(Setup) == 8);
    static_assert(std::endian::native == std::endian::little);

    void CheckSocket(bool failed)
    {
        THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), failed);
    }

    bool Receive(SOCKET socket, void* buffer, size_t size)
    {
        auto* cursor = static_cast<char*>(buffer);
        while (size != 0)
        {
            const auto count = recv(socket, cursor, gsl::narrow<int>(size), 0);
            CheckSocket(count == SOCKET_ERROR);
            if (count == 0)
            {
                return false;
            }
            cursor += count;
            size -= count;
        }
        return true;
    }

    void Send(SOCKET socket, const void* buffer, size_t size)
    {
        auto* cursor = static_cast<const char*>(buffer);
        while (size != 0)
        {
            const auto count = send(socket, cursor, gsl::narrow<int>(size), 0);
            CheckSocket(count == SOCKET_ERROR);
            THROW_WIN32_IF(ERROR_BROKEN_PIPE, count == 0);
            cursor += count;
            size -= count;
        }
    }

    void ReplyOperation(SOCKET socket, uint16_t code, uint32_t status = 0)
    {
        const Operation reply{htons(c_version), htons(code), htonl(status)};
        Send(socket, &reply, sizeof(reply));
    }

    void Describe(SOCKET socket, const Profile& profile, std::string_view busId, bool list)
    {
        Export info{};
        const auto path = std::format("/sys/devices/virtual/usb1/{}", busId);
        THROW_HR_IF(E_INVALIDARG, busId.size() >= sizeof(info.BusId) || path.size() >= sizeof(info.Path));
        std::ranges::copy(path, info.Path);
        std::ranges::copy(busId, info.BusId);
        info.Bus = htonl(1);
        info.Number = htonl(profile.ProductId);
        info.Speed = htonl(2);
        info.Vendor = htons(c_vendorId);
        info.Product = htons(profile.ProductId);
        info.Release = htons(0x0100);
        info.Class = profile.DeviceClass;
        info.Subclass = profile.DeviceSubclass;
        info.Configuration = info.Configurations = 1;
        info.Interfaces = gsl::narrow<uint8_t>(profile.Interfaces.size());
        Send(socket, &info, sizeof(info));
        if (list)
        {
            Send(socket, profile.Interfaces.data(), profile.Interfaces.size_bytes());
        }
    }

    using Bytes = std::vector<uint8_t>;

    Bytes StringDescriptor(std::string_view text)
    {
        Bytes bytes{gsl::narrow<uint8_t>(2 + 2 * text.size()), USB_STRING_DESCRIPTOR_TYPE};
        for (const auto character : text)
        {
            bytes.push_back(static_cast<uint8_t>(character));
            bytes.push_back(0);
        }
        return bytes;
    }

    std::optional<Bytes> Descriptor(const Profile& profile, const std::string& serial, uint16_t value)
    {
        switch (value)
        {
        case USB_DEVICE_DESCRIPTOR_TYPE << 8:
            return Bytes{
                18,
                USB_DEVICE_DESCRIPTOR_TYPE,
                0,
                2,
                profile.DeviceClass,
                profile.DeviceSubclass,
                0,
                64,
                static_cast<uint8_t>(c_vendorId & 0xff),
                static_cast<uint8_t>(c_vendorId >> 8),
                static_cast<uint8_t>(profile.ProductId),
                0,
                0,
                1,
                1,
                2,
                3,
                1};
        case USB_CONFIGURATION_DESCRIPTOR_TYPE << 8:
            return Bytes{profile.Config.begin(), profile.Config.end()};
        case (USB_STRING_DESCRIPTOR_TYPE << 8):
            return Bytes{4, USB_STRING_DESCRIPTOR_TYPE, 0x09, 0x04};
        case (USB_STRING_DESCRIPTOR_TYPE << 8) | 1:
            return StringDescriptor("WSL");
        case (USB_STRING_DESCRIPTOR_TYPE << 8) | 2:
            return StringDescriptor(profile.Name);
        case (USB_STRING_DESCRIPTOR_TYPE << 8) | 3:
            return StringDescriptor(serial);
        default:
            return {};
        }
    }

    class Session
    {
    public:
        Session(SOCKET socket, Device device, const std::string& serial) : m_socket(socket), m_device(device), m_serial(serial)
        {
        }

        void Run()
        {
            Transfer request{};
            while (Receive(m_socket, &request, sizeof(request)))
            {
                if (ntohl(request.Command) == c_unlink)
                {
                    const auto removed =
                        std::erase_if(m_pending, [&](const auto& entry) { return entry.Sequence == request.StatusOrFlags; });
                    Reply(request.Sequence, c_unlinkReply, removed ? c_cancelled : 0, 0);
                    continue;
                }

                const size_t length = ntohl(request.Length);
                const auto direction = ntohl(request.Direction);
                THROW_HR_IF(
                    E_INVALIDARG,
                    ntohl(request.Command) != c_submit || length > c_maxTransfer || direction > 1 ||
                        (request.Packets != 0 && request.Packets != UINT32_MAX));
                Bytes payload(direction == 0 ? length : 0);
                if (!Receive(m_socket, payload.data(), payload.size()))
                {
                    return;
                }

                const auto endpoint = ntohl(request.Endpoint);
                if (endpoint == 0)
                {
                    auto response = Control(request.Control, payload);
                    if (!response)
                    {
                        Reply(request.Sequence, c_submitReply, c_stall, 0);
                        continue;
                    }
                    response->resize(std::min({response->size(), length, size_t{request.Control.Length}}));
                    Reply(request.Sequence, c_submitReply, 0, direction ? response->size() : payload.size(), *response);
                }
                else if (m_device == Device::Serial && endpoint == 2 && direction == 0)
                {
                    if (length > c_maxTransfer - m_fifo.size())
                    {
                        Reply(request.Sequence, c_submitReply, c_noSpace, 0);
                        continue;
                    }
                    m_fifo.insert(m_fifo.end(), payload.begin(), payload.end());
                    Reply(request.Sequence, c_submitReply, 0, length);
                    CompleteReads();
                }
                else if (m_device == Device::Serial && direction == 1 && (endpoint == 1 || endpoint == 2))
                {
                    if (length == 0 || m_pending.size() >= 64)
                    {
                        Reply(request.Sequence, c_submitReply, length ? c_noSpace : 0, 0);
                        continue;
                    }
                    m_pending.push_back({request.Sequence, length, endpoint == 2});
                    CompleteReads();
                }
                else
                {
                    Reply(request.Sequence, c_submitReply, c_stall, 0);
                }
            }
        }

    private:
        struct Pending
        {
            uint32_t Sequence;
            size_t Length;
            bool Bulk;
        };

        void Reply(uint32_t sequence, uint32_t command, int status, size_t length, std::span<const uint8_t> data = {}) const
        {
            Transfer reply{};
            reply.Command = htonl(command);
            reply.Sequence = sequence;
            reply.StatusOrFlags = htonl(static_cast<uint32_t>(status));
            reply.Length = htonl(gsl::narrow<uint32_t>(length));
            Send(m_socket, &reply, sizeof(reply));
            Send(m_socket, data.data(), data.size());
        }

        void CompleteReads()
        {
            for (auto entry = m_pending.begin(); entry != m_pending.end() && !m_fifo.empty();)
            {
                if (!entry->Bulk)
                {
                    ++entry;
                    continue;
                }
                const auto end = m_fifo.begin() + std::min(entry->Length, m_fifo.size());
                const Bytes response(m_fifo.begin(), end);
                Reply(entry->Sequence, c_submitReply, 0, response.size(), response);
                m_fifo.erase(m_fifo.begin(), end);
                entry = m_pending.erase(entry);
            }
        }

        std::optional<Bytes> Control(const Setup& setup, const Bytes& payload)
        {
            if (setup.Type == 0x80 && setup.Request == USB_REQUEST_GET_DESCRIPTOR)
            {
                return Descriptor(GetProfile(m_device), m_serial, setup.Value);
            }
            if ((setup.Type == 0x80 || setup.Type == 0x81 || setup.Type == 0x82) && setup.Request == USB_REQUEST_GET_STATUS)
            {
                return Bytes{0, 0};
            }
            if (setup.Type == 0x80 && setup.Request == USB_REQUEST_GET_CONFIGURATION)
            {
                return Bytes{m_configuration};
            }
            if (setup.Type == 0 && setup.Request == USB_REQUEST_SET_CONFIGURATION && setup.Value <= 1)
            {
                m_configuration = static_cast<uint8_t>(setup.Value);
                return Bytes{};
            }
            if (setup.Type == 0x81 && setup.Request == USB_REQUEST_GET_INTERFACE)
            {
                return Bytes{0};
            }
            if ((setup.Type == 1 && setup.Request == USB_REQUEST_SET_INTERFACE && setup.Value == 0) ||
                (setup.Type == 2 && setup.Request == USB_REQUEST_CLEAR_FEATURE && setup.Value == 0))
            {
                return Bytes{};
            }
            if (m_device == Device::Serial && setup.Index == 0)
            {
                if (setup.Type == 0xa1 && setup.Request == 0x21)
                {
                    return Bytes(m_lineCoding.begin(), m_lineCoding.end());
                }
                if (setup.Type == 0x21 && setup.Request == 0x20 && payload.size() == m_lineCoding.size())
                {
                    std::ranges::copy(payload, m_lineCoding.begin());
                    return Bytes{};
                }
                if (setup.Type == 0x21 && setup.Request == 0x22)
                {
                    return Bytes{};
                }
            }
            return {};
        }

        SOCKET m_socket;
        Device m_device;
        const std::string& m_serial;
        uint8_t m_configuration{1};
        std::array<uint8_t, 7> m_lineCoding{0, 0xc2, 1, 0, 0, 0, 8};
        std::deque<uint8_t> m_fifo;
        std::deque<Pending> m_pending;
    };

    struct Winsock
    {
        Winsock()
        {
            WSADATA data{};
            THROW_IF_WIN32_ERROR(static_cast<DWORD>(WSAStartup(MAKEWORD(2, 2), &data)));
        }

        ~Winsock()
        {
            WSACleanup();
        }
    };

    struct Connection
    {
        explicit Connection(wil::unique_socket socket) : Socket(std::move(socket))
        {
        }

        ~Connection()
        {
            if (Worker.joinable())
            {
                shutdown(Socket.get(), SD_BOTH);
                Worker.join();
            }
        }

        wil::unique_socket Socket;
        std::atomic<bool> Finished{false};
        std::thread Worker;
    };

} // namespace

struct Server::Impl
{
    struct ImportSlot
    {
        std::string ReservedBusId;
        SOCKET Client{INVALID_SOCKET};

        std::string_view BusId(const Profile& profile) const
        {
            return ReservedBusId.empty() ? profile.BusId : std::string_view{ReservedBusId};
        }
    };

    Impl()
    {
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        Serial = std::format("WSL{:08X}{:04X}{:04X}", id.Data1, id.Data2, id.Data3);
        Listener.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        CheckSocket(!Listener);
        const BOOL exclusive = TRUE;
        CheckSocket(setsockopt(Listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CheckSocket(bind(Listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
        CheckSocket(listen(Listener.get(), SOMAXCONN) == SOCKET_ERROR);
        int size = sizeof(address);
        CheckSocket(getsockname(Listener.get(), reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR);
        Port = ntohs(address.sin_port);
        Acceptor = std::jthread([this](std::stop_token stop) {
            try
            {
                std::vector<std::unique_ptr<Connection>> connections;
                while (!stop.stop_requested())
                {
                    std::erase_if(connections, [](const auto& entry) { return entry->Finished.load(); });
                    WSAPOLLFD poll{Listener.get(), POLLRDNORM, 0};
                    const auto result = WSAPoll(&poll, 1, 100);
                    CheckSocket(result == SOCKET_ERROR);
                    if (!result)
                    {
                        continue;
                    }
                    wil::unique_socket client(accept(Listener.get(), nullptr, nullptr));
                    if (!client)
                    {
                        const auto error = WSAGetLastError();
                        if (error == WSAECONNRESET || error == WSAECONNABORTED)
                        {
                            continue;
                        }
                        THROW_WIN32(static_cast<DWORD>(error));
                    }
                    if (connections.size() < 8)
                    {
                        auto& peer = connections.emplace_back(std::make_unique<Connection>(std::move(client)));
                        peer->Worker = std::thread([this, connection = peer.get()] {
                            try
                            {
                                Serve(connection->Socket.get());
                            }
                            CATCH_LOG()
                            connection->Finished.store(true);
                        });
                    }
                }
            }
            CATCH_LOG()
        });
    }

    void Serve(SOCKET socket)
    {
        Operation op{};
        if (!Receive(socket, &op, sizeof(op)) || ntohs(op.Version) != c_version || op.Status != 0)
        {
            return;
        }
        constexpr std::array devices{Device::Serial, Device::Audio};
        if (ntohs(op.Code) == (c_request | c_list))
        {
            ReplyOperation(socket, c_list);
            const auto count = htonl(static_cast<uint32_t>(devices.size()));
            Send(socket, &count, sizeof(count));
            for (const auto device : devices)
            {
                const auto& profile = GetProfile(device);
                std::string busId;
                {
                    auto lock = ImportLock.lock_shared();
                    busId = Imports[static_cast<size_t>(device)].BusId(profile);
                }
                Describe(socket, profile, busId, true);
            }
        }
        else if (ntohs(op.Code) == (c_request | c_import))
        {
            std::array<char, 32> bus{};
            if (!Receive(socket, bus.data(), bus.size()))
            {
                return;
            }
            const std::string_view busId(bus.data(), std::ranges::find(bus, '\0') - bus.begin());
            for (const auto device : devices)
            {
                const auto& profile = GetProfile(device);
                auto& slot = Imports[static_cast<size_t>(device)];
                {
                    auto lock = ImportLock.lock_exclusive();
                    if (slot.BusId(profile) != busId || slot.Client != INVALID_SOCKET)
                    {
                        continue;
                    }
                    slot.Client = socket;
                }
                auto release = wil::scope_exit([&] {
                    auto lock = ImportLock.lock_exclusive();
                    if (slot.Client == socket)
                    {
                        slot.Client = INVALID_SOCKET;
                    }
                });
                ReplyOperation(socket, c_import);
                Describe(socket, profile, busId, false);
                Session(socket, device, Serial).Run();
                return;
            }
            ReplyOperation(socket, c_import, 1);
        }
    }

    Winsock Network;
    wil::unique_socket Listener;
    wil::srwlock ImportLock;
    _Guarded_by_(ImportLock) std::array<ImportSlot, 2> Imports {};
    std::string Serial;
    uint16_t Port{};
    std::jthread Acceptor;
};

Server::Server() : m_impl(std::make_unique<Impl>())
{
}

Server::~Server() = default;

uint16_t Server::Port() const
{
    return m_impl->Port;
}

std::wstring Server::SerialNumber() const
{
    return {m_impl->Serial.begin(), m_impl->Serial.end()};
}

std::wstring Server::InstanceId(Device device) const
{
    return std::format(L"USB\\VID_{:04X}&PID_{:04X}\\{}", c_vendorId, GetProfile(device).ProductId, SerialNumber());
}

std::string Server::ReserveImport(Device device) const
{
    GUID id{};
    THROW_IF_FAILED(CoCreateGuid(&id));
    auto busId = std::format("{}-{:08x}{:04x}{:04x}", GetProfile(device).BusId, id.Data1, id.Data2, id.Data3);
    auto lock = m_impl->ImportLock.lock_exclusive();
    auto& slot = m_impl->Imports[static_cast<size_t>(device)];
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), !slot.ReservedBusId.empty() || slot.Client != INVALID_SOCKET);
    slot.ReservedBusId = busId;
    return busId;
}

void Server::WithdrawImport(Device device, std::string_view busId) const
{
    auto lock = m_impl->ImportLock.lock_exclusive();
    auto& slot = m_impl->Imports[static_cast<size_t>(device)];
    if (slot.ReservedBusId == busId)
    {
        slot.ReservedBusId.clear();
        if (slot.Client != INVALID_SOCKET)
        {
            shutdown(std::exchange(slot.Client, INVALID_SOCKET), SD_BOTH);
        }
    }
}

} // namespace wsl::test::usbip