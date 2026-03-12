#pragma once

#include "pch.h"
#include "Socket.h"
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Transcoding.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <vector>
#include <functional>
#include <memory>

namespace winrt::WSLAMoviePlayer::implementation
{
    struct SubtitleSegment {
        winrt::hstring nativeText;
        winrt::hstring translatedText;
        int64_t startTime;  // milliseconds
        int64_t endTime;    // milliseconds
    };

    struct CoverageRange {
        int64_t start;
        int64_t end;
    };

    class Subtitler
    {
    public:
        Subtitler();
        ~Subtitler();

        void UpdatePosition(int64_t positionMs);
        void Reset();
        winrt::Windows::Foundation::IAsyncAction SeekAsync(int64_t timeMs);

        // Get coverage info
        const std::vector<SubtitleSegment>& GetSubtitles() const { return m_subtitles; }
        const std::vector<CoverageRange>& GetCoverageRanges() const { return m_coverageRanges; }
        bool HasSubtitleAt(int64_t timeMs);

        // Video source for audio extraction
        winrt::Windows::Foundation::IAsyncAction SetVideoSourceAsync(const winrt::Windows::Storage::StorageFile& file);

        // Event callbacks
        std::function<void(const winrt::hstring&)> OnNativeSubtitleChanged;
        std::function<void(const winrt::hstring&)> OnTranslatedSubtitleChanged;
        std::function<void(double)> OnProcessingStarted;
        std::function<void()> OnProcessingFinished;
        std::function<void()> OnSubtitlesUpdated;
        std::function<void()> OnConnectionEstablished;
        std::function<void()> OnConnectionLost;
        std::function<void(const winrt::hstring&)> OnConnectionError;

        // Initialize the socket connection
        winrt::Windows::Foundation::IAsyncAction InitializeAsync();

    private:
        bool m_isProcessing;
        winrt::hstring m_detectedLanguage;

        // Subtitle storage (ordered by start time)
        std::vector<SubtitleSegment> m_subtitles;

        // Coverage tracking (list of [startTime, endTime] ranges in milliseconds)
        std::vector<CoverageRange> m_coverageRanges;

        // Current subtitle display
        winrt::hstring m_currentNativeText;
        winrt::hstring m_currentTranslatedText;

        // WebSocket
        std::unique_ptr<Socket> m_socket;
        
        // Audio data
        winrt::Windows::Storage::Streams::IBuffer m_audioBuffer{ nullptr };
        int64_t m_audioDuration; // Duration in milliseconds
        bool m_audioLoaded;

        SubtitleSegment* FindSubtitleAt(int64_t timeMs);
        
        // Coverage range helpers
        bool IsPositionCovered(int64_t timeMs) const;
        void AddOrExtendCoverageRange(int64_t endTime);
        void StartNewCoverageRange(int64_t startTime);

        // Socket event handlers
        void OnSocketConnected();
        void OnSocketDisconnected();
        void OnSocketError(const winrt::hstring& errorString);
        void OnSocketProcessingStarted(const ProcessingStartedEvent& event);
        void OnSocketSubtitleReceived(const SubtitleEvent& event);
        void OnSocketTranscriptionCompleted(const CompletedEvent& event);
        void OnSocketAudioLoaded();

        // Audio extraction
        winrt::Windows::Foundation::IAsyncAction ExtractAudioAsync(const winrt::Windows::Storage::StorageFile& videoFile);
    };
}
