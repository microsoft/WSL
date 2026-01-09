#include "pch.h"
#include "Socket.h"
#include <winrt/Windows.Data.Json.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage::Streams;
using namespace Windows::Data::Json;

namespace winrt::WSLAMoviePlayer::implementation
{
    Socket::Socket()
        : m_url(L"ws://localhost:8000/ws")
        , m_isConnected(false)
    {
        OutputDebugStringW(L"Socket: Creating new Socket instance\n");
    }

    Socket::~Socket()
    {
        OutputDebugStringW(L"Socket: Destroying Socket instance\n");
        Disconnect();
    }

    void Socket::SetUrl(const hstring& url)
    {
        m_url = url;
    }

    IAsyncAction Socket::ConnectAsync()
    {
        if (m_isConnected)
        {
            OutputDebugStringW(L"Socket: Already connected\n");
            co_return;
        }

        try
        {
            OutputDebugStringW((L"Socket: Connecting to " + m_url + L"\n").c_str());
            
            m_webSocket = MessageWebSocket();
            m_webSocket.Control().MessageType(SocketMessageType::Utf8);
            
            m_webSocket.MessageReceived({ this, &Socket::HandleMessageReceived });
            m_webSocket.Closed({ this, &Socket::HandleClosed });

            Uri uri(m_url);
            co_await m_webSocket.ConnectAsync(uri);
            
            m_writer = DataWriter(m_webSocket.OutputStream());
            m_isConnected = true;
            
            OutputDebugStringW(L"Socket: Connected successfully\n");
            
            if (OnConnected)
            {
                OnConnected();
            }
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Connection error - " + ex.message() + L"\n").c_str());
            m_isConnected = false;
            if (OnError)
            {
                OnError(ex.message());
            }
        }
    }

    void Socket::Disconnect()
    {
        if (m_webSocket)
        {
            try
            {
                m_webSocket.Close(1000, L"Normal closure");
            }
            catch (...) {}
            m_webSocket = nullptr;
            m_writer = nullptr;
        }
        m_isConnected = false;
    }

    bool Socket::IsConnected() const
    {
        return m_isConnected;
    }

    IAsyncAction Socket::LoadAudioAsync(const IBuffer& audioData)
    {
        if (!m_isConnected || !m_writer)
        {
            OutputDebugStringW(L"Socket: Cannot load audio - not connected\n");
            co_return;
        }

        try
        {
            uint32_t totalSize = audioData.Length();
            constexpr uint32_t CHUNK_SIZE = 256 * 1024; // 256KB chunks
            uint32_t totalChunks = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;

            OutputDebugStringW((L"Socket: Streaming audio in " + to_hstring(totalChunks) + L" chunks\n").c_str());

            // 1. Send metadata message first
            JsonObject startJson;
            startJson.SetNamedValue(L"type", JsonValue::CreateStringValue(L"load_start"));
            startJson.SetNamedValue(L"total_size", JsonValue::CreateNumberValue(static_cast<double>(totalSize)));
            startJson.SetNamedValue(L"chunk_count", JsonValue::CreateNumberValue(static_cast<double>(totalChunks)));
            
            co_await SendTextMessageAsync(startJson.Stringify());

            // 2. Send binary chunks
            auto dataReader = DataReader::FromBuffer(audioData);
            uint32_t offset = 0;
            uint32_t chunkNum = 0;

            // Switch to binary mode for sending chunks
            m_webSocket.Control().MessageType(SocketMessageType::Binary);

            while (offset < totalSize)
            {
                uint32_t chunkSize = (std::min)(CHUNK_SIZE, totalSize - offset);
                
                // Read chunk data
                std::vector<uint8_t> chunkData(chunkSize);
                dataReader.ReadBytes(chunkData);
                
                // Write the chunk
                m_writer.WriteBytes(chunkData);
                co_await m_writer.StoreAsync();

                chunkNum++;
                offset += chunkSize;

                if (chunkNum % 10 == 0 || chunkNum == totalChunks)
                {
                    OutputDebugStringW((L"Socket: Sent chunk " + to_hstring(chunkNum) + L"/" + to_hstring(totalChunks) + L"\n").c_str());
                }
            }

            // Switch back to text mode
            m_webSocket.Control().MessageType(SocketMessageType::Utf8);

            // 3. Send completion message
            JsonObject completeJson;
            completeJson.SetNamedValue(L"type", JsonValue::CreateStringValue(L"load_complete"));
            completeJson.SetNamedValue(L"total_size", JsonValue::CreateNumberValue(static_cast<double>(totalSize)));
            
            co_await SendTextMessageAsync(completeJson.Stringify());

            OutputDebugStringW((L"Socket: Successfully streamed " + to_hstring(totalSize / 1024) + L" KB\n").c_str());
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Error loading audio - " + ex.message() + L"\n").c_str());
            if (OnError)
            {
                OnError(ex.message());
            }
        }
    }

    IAsyncAction Socket::SeekAsync(double seekTime)
    {
        if (!m_isConnected)
        {
            OutputDebugStringW(L"Socket: Cannot seek - not connected\n");
            co_return;
        }

        try
        {
            JsonObject json;
            json.SetNamedValue(L"type", JsonValue::CreateStringValue(L"seek"));
            json.SetNamedValue(L"seek_time", JsonValue::CreateNumberValue(seekTime));
            
            OutputDebugStringW((L"Socket: Sending seek command for time: " + to_hstring(seekTime) + L"\n").c_str());
            co_await SendTextMessageAsync(json.Stringify());
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Error seeking - " + ex.message() + L"\n").c_str());
        }
    }

    IAsyncAction Socket::SendTextMessageAsync(const hstring& message)
    {
        if (!m_writer)
        {
            co_return;
        }

        try
        {
            m_webSocket.Control().MessageType(SocketMessageType::Utf8);
            m_writer.WriteString(message);
            co_await m_writer.StoreAsync();
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Error sending text - " + ex.message() + L"\n").c_str());
            throw;
        }
    }

    void Socket::HandleMessageReceived(
        MessageWebSocket const& /* sender */,
        MessageWebSocketMessageReceivedEventArgs const& args)
    {
        try
        {
            auto reader = args.GetDataReader();
            reader.UnicodeEncoding(UnicodeEncoding::Utf8);
            
            hstring message = reader.ReadString(reader.UnconsumedBufferLength());
            OutputDebugStringW((L"Socket: Received message: " + message + L"\n").c_str());
            
            ParseJsonMessage(message);
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Error reading message - " + ex.message() + L"\n").c_str());
        }
    }

    void Socket::HandleClosed(
        IWebSocket const& /* sender */,
        WebSocketClosedEventArgs const& args)
    {
        OutputDebugStringW((L"Socket: Connection closed - " + args.Reason() + L"\n").c_str());
        m_isConnected = false;
        
        if (OnDisconnected)
        {
            OnDisconnected();
        }
    }

    void Socket::ParseJsonMessage(const hstring& message)
    {
        try
        {
            auto json = JsonObject::Parse(message);
            hstring type = json.GetNamedString(L"type", L"");

            if (type == L"processing_started")
            {
                ProcessingStartedEvent event;
                event.seek_time = json.GetNamedNumber(L"seek_time", 0.0);
                OutputDebugStringW((L"Socket: Processing started at seek_time: " + to_hstring(event.seek_time) + L"\n").c_str());
                if (OnProcessingStarted)
                {
                    OnProcessingStarted(event);
                }
            }
            else if (type == L"subtitle")
            {
                SubtitleEvent event;
                event.start = json.GetNamedNumber(L"start", 0.0);
                event.end = json.GetNamedNumber(L"end", 0.0);
                event.text = json.GetNamedString(L"text", L"");
                event.text_en = json.GetNamedString(L"text_en", L"");
                event.language = json.GetNamedString(L"language", L"");
                OutputDebugStringW((L"Socket: Subtitle: " + event.text + L"\n").c_str());
                if (OnSubtitleReceived)
                {
                    OnSubtitleReceived(event);
                }
            }
            else if (type == L"completed")
            {
                CompletedEvent event;
                event.full_text = json.GetNamedString(L"full_text", L"");
                event.full_text_en = json.GetNamedString(L"full_text_en", L"");
                event.language = json.GetNamedString(L"language", L"");
                event.seek_time = json.GetNamedNumber(L"seek_time", 0.0);
                OutputDebugStringW(L"Socket: Transcription completed\n");
                if (OnTranscriptionCompleted)
                {
                    OnTranscriptionCompleted(event);
                }
            }
            else if (type == L"processing_cancelled")
            {
                double seekTime = json.GetNamedNumber(L"seek_time", 0.0);
                OutputDebugStringW(L"Socket: Processing cancelled\n");
                if (OnProcessingCancelled)
                {
                    OnProcessingCancelled(seekTime);
                }
            }
            else if (type == L"loaded")
            {
                OutputDebugStringW(L"Socket: Audio loaded on server\n");
                if (OnAudioLoaded)
                {
                    OnAudioLoaded();
                }
            }
            else if (type == L"error")
            {
                hstring errorMsg = json.GetNamedString(L"message", L"Unknown error");
                OutputDebugStringW((L"Socket: Server error - " + errorMsg + L"\n").c_str());
                if (OnError)
                {
                    OnError(errorMsg);
                }
            }
            else
            {
                OutputDebugStringW((L"Socket: Unknown event type: " + type + L"\n").c_str());
            }
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Socket: Error parsing JSON - " + ex.message() + L"\n").c_str());
        }
    }
}
