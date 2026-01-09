#pragma once

#include "pch.h"
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Json.h>
#include <string>
#include <functional>

namespace winrt::WSLAMoviePlayer::implementation
{
    // Event data structures
    struct SubtitleEvent {
        double start;
        double end;
        winrt::hstring text;
        winrt::hstring text_en;
        winrt::hstring language;
    };

    struct CompletedEvent {
        winrt::hstring full_text;
        winrt::hstring full_text_en;
        winrt::hstring language;
        double seek_time;
    };

    struct ProcessingStartedEvent {
        double seek_time;
    };

    class Socket
    {
    public:
        Socket();
        ~Socket();

        void SetUrl(const winrt::hstring& url);
        winrt::Windows::Foundation::IAsyncAction ConnectAsync();
        void Disconnect();
        bool IsConnected() const;

        // Send commands to server
        winrt::Windows::Foundation::IAsyncAction LoadAudioAsync(const winrt::Windows::Storage::Streams::IBuffer& audioData);
        winrt::Windows::Foundation::IAsyncAction SeekAsync(double seekTime);

        // Event handlers (use std::function for simplicity)
        std::function<void()> OnConnected;
        std::function<void()> OnDisconnected;
        std::function<void(const winrt::hstring&)> OnError;
        std::function<void(const ProcessingStartedEvent&)> OnProcessingStarted;
        std::function<void(const SubtitleEvent&)> OnSubtitleReceived;
        std::function<void(const CompletedEvent&)> OnTranscriptionCompleted;
        std::function<void(double)> OnProcessingCancelled;
        std::function<void()> OnAudioLoaded;

    private:
        winrt::Windows::Networking::Sockets::MessageWebSocket m_webSocket{ nullptr };
        winrt::Windows::Storage::Streams::DataWriter m_writer{ nullptr };
        winrt::hstring m_url;
        bool m_isConnected;

        void HandleMessageReceived(
            winrt::Windows::Networking::Sockets::MessageWebSocket const& sender,
            winrt::Windows::Networking::Sockets::MessageWebSocketMessageReceivedEventArgs const& args);
        void HandleClosed(
            winrt::Windows::Networking::Sockets::IWebSocket const& sender,
            winrt::Windows::Networking::Sockets::WebSocketClosedEventArgs const& args);
        void ParseJsonMessage(const winrt::hstring& message);
        winrt::Windows::Foundation::IAsyncAction SendTextMessageAsync(const winrt::hstring& message);
    };
}
