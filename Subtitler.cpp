#include "pch.h"
#include "Subtitler.h"
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Transcoding.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <sysinfoapi.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Media::Transcoding;

namespace winrt::WSLAMoviePlayer::implementation
{
    Subtitler::Subtitler()
        : m_isProcessing(false)
        , m_audioDuration(0)
        , m_audioLoaded(false)
    {
        OutputDebugStringW(L"Subtitler: Creating new Subtitler instance\n");
        
        m_socket = std::make_unique<Socket>();
        
        // Set up socket event handlers
        m_socket->OnConnected = [this]() { OnSocketConnected(); };
        m_socket->OnDisconnected = [this]() { OnSocketDisconnected(); };
        m_socket->OnError = [this](const hstring& error) { OnSocketError(error); };
        m_socket->OnProcessingStarted = [this](const ProcessingStartedEvent& event) { OnSocketProcessingStarted(event); };
        m_socket->OnSubtitleReceived = [this](const SubtitleEvent& event) { OnSocketSubtitleReceived(event); };
        m_socket->OnTranscriptionCompleted = [this](const CompletedEvent& event) { OnSocketTranscriptionCompleted(event); };
        m_socket->OnAudioLoaded = [this]() { OnSocketAudioLoaded(); };
    }

    Subtitler::~Subtitler()
    {
        OutputDebugStringW(L"Subtitler: Destroying Subtitler instance\n");
        if (m_socket)
        {
            m_socket->Disconnect();
        }
    }

    IAsyncAction Subtitler::InitializeAsync()
    {
        OutputDebugStringW(L"Subtitler: Initializing socket connection\n");
        co_await m_socket->ConnectAsync();
        co_return;
    }

    void Subtitler::UpdatePosition(int64_t positionMs)
    {
        SubtitleSegment* segment = FindSubtitleAt(positionMs);

        if (segment)
        {
            // Update subtitle if changed
            if (m_currentNativeText != segment->nativeText || m_currentTranslatedText != segment->translatedText)
            {
                m_currentNativeText = segment->nativeText;
                m_currentTranslatedText = segment->translatedText;
                
                if (OnNativeSubtitleChanged)
                {
                    OnNativeSubtitleChanged(segment->nativeText);
                }
                if (OnTranslatedSubtitleChanged)
                {
                    OnTranslatedSubtitleChanged(segment->translatedText);
                }
            }
        }
        else
        {
            // Clear subtitles if no subtitle at current position
            if (!m_currentNativeText.empty() || !m_currentTranslatedText.empty())
            {
                m_currentNativeText = L"";
                m_currentTranslatedText = L"";
                
                if (OnNativeSubtitleChanged)
                {
                    OnNativeSubtitleChanged(L"");
                }
                if (OnTranslatedSubtitleChanged)
                {
                    OnTranslatedSubtitleChanged(L"");
                }
            }
        }
    }

    void Subtitler::Reset()
    {
        OutputDebugStringW(L"Subtitler: Resetting subtitles\n");
        m_subtitles.clear();
        m_coverageRanges.clear();
        m_currentNativeText = L"";
        m_currentTranslatedText = L"";
        m_detectedLanguage = L"";
        m_audioLoaded = false;

        if (OnNativeSubtitleChanged)
        {
            OnNativeSubtitleChanged(L"");
        }
        if (OnTranslatedSubtitleChanged)
        {
            OnTranslatedSubtitleChanged(L"");
        }
        if (OnSubtitlesUpdated)
        {
            OnSubtitlesUpdated();
        }
    }

    bool Subtitler::HasSubtitleAt(int64_t timeMs)
    {
        return FindSubtitleAt(timeMs) != nullptr;
    }

    IAsyncAction Subtitler::SeekAsync(int64_t timeMs)
    {
        if (!m_socket || !m_socket->IsConnected())
        {
            OutputDebugStringW(L"Subtitler: Cannot seek - socket not connected\n");
            co_return;
        }

        if (!m_audioLoaded)
        {
            OutputDebugStringW(L"Subtitler: Cannot seek - audio not loaded yet\n");
            co_return;
        }

        // Check if this position is already covered
        if (IsPositionCovered(timeMs))
        {
            OutputDebugStringW((L"Subtitler: Position " + to_hstring(timeMs) + L" already covered\n").c_str());
            co_return;
        }

        // Start a new coverage range at this position
        StartNewCoverageRange(timeMs);

        // Convert milliseconds to seconds
        double seekTime = timeMs / 1000.0;
        OutputDebugStringW((L"Subtitler: Seeking to " + to_hstring(seekTime) + L" seconds\n").c_str());
        co_await m_socket->SeekAsync(seekTime);
        co_return;
    }

    SubtitleSegment* Subtitler::FindSubtitleAt(int64_t timeMs)
    {
        for (SubtitleSegment& segment : m_subtitles)
        {
            if (timeMs >= segment.startTime && timeMs < segment.endTime)
            {
                return &segment;
            }
        }
        return nullptr;
    }

    IAsyncAction Subtitler::SetVideoSourceAsync(const StorageFile& file)
    {
        Reset();

        OutputDebugStringW((L"Subtitler: Setting video source: " + file.Path() + L"\n").c_str());

        // Extract audio from video file
        co_await ExtractAudioAsync(file);

        // Start initial coverage range at position 0
        StartNewCoverageRange(0);
        co_return;
    }

    IAsyncAction Subtitler::ExtractAudioAsync(const StorageFile& videoFile)
    {
        OutputDebugStringW(L"Subtitler: Starting audio extraction\n");

        try
        {
            // Get video duration first using MediaSource
            auto mediaSource = MediaSource::CreateFromStorageFile(videoFile);
            co_await mediaSource.OpenAsync();

            if (mediaSource.Duration())
            {
                m_audioDuration = std::chrono::duration_cast<std::chrono::milliseconds>(mediaSource.Duration().Value()).count();
                OutputDebugStringW((L"Subtitler: Video duration: " + to_hstring(m_audioDuration) + L" ms\n").c_str());
            }
            mediaSource.Close();

            // Create a temporary output file for the extracted audio
            auto tempFolder = Windows::Storage::ApplicationData::Current().TemporaryFolder();
            hstring tempFileName = L"temp_audio_" + to_hstring(GetTickCount64()) + L".wav";
            auto outputFile = co_await tempFolder.CreateFileAsync(tempFileName, Windows::Storage::CreationCollisionOption::ReplaceExisting);

            OutputDebugStringW((L"Subtitler: Transcoding audio to: " + outputFile.Path() + L"\n").c_str());

            // Set up transcoding to extract audio as WAV (16kHz, 16-bit, mono)
            MediaTranscoder transcoder;
            
            // Create audio encoding profile: 16kHz, 16-bit, Mono WAV
            auto audioProfile = MediaEncodingProfile::CreateWav(AudioEncodingQuality::Low);
            audioProfile.Audio().SampleRate(16000);  // 16kHz
            audioProfile.Audio().BitsPerSample(16);  // 16-bit
            audioProfile.Audio().ChannelCount(1);    // Mono
            audioProfile.Video(nullptr);  // No video

            // Prepare transcode
            auto prepareResult = co_await transcoder.PrepareFileTranscodeAsync(videoFile, outputFile, audioProfile);

            if (!prepareResult.CanTranscode())
            {
                OutputDebugStringW((L"Subtitler: Cannot transcode - " + to_hstring(static_cast<int>(prepareResult.FailureReason())) + L"\n").c_str());
                co_return;
            }

            OutputDebugStringW(L"Subtitler: Starting transcoding...\n");

            // Perform the transcode
            co_await prepareResult.TranscodeAsync();

            OutputDebugStringW(L"Subtitler: Transcoding completed\n");

            // Read the transcoded audio file
            auto audioStream = co_await outputFile.OpenReadAsync();
            auto audioSize = static_cast<uint32_t>(audioStream.Size());

            OutputDebugStringW((L"Subtitler: Transcoded audio size: " + to_hstring(audioSize / 1024) + L" KB\n").c_str());

            auto dataReader = DataReader(audioStream);
            co_await dataReader.LoadAsync(audioSize);

            m_audioBuffer = dataReader.DetachBuffer();
            OutputDebugStringW(L"Subtitler: Audio loaded into buffer\n");

            // Clean up temp file
            try
            {
                co_await outputFile.DeleteAsync();
            }
            catch (...) {}

            // Send audio to server
            if (m_socket && m_socket->IsConnected())
            {
                OutputDebugStringW(L"Subtitler: Sending audio to server\n");
                co_await m_socket->LoadAudioAsync(m_audioBuffer);
            }
            else
            {
                OutputDebugStringW(L"Subtitler: Socket not connected, cannot send audio\n");
            }
        }
        catch (hresult_error const& ex)
        {
            OutputDebugStringW((L"Subtitler: Error extracting audio - " + ex.message() + L"\n").c_str());
        }

        co_return;
    }

    void Subtitler::OnSocketConnected()
    {
        OutputDebugStringW(L"Subtitler: Socket connected\n");
    }

    void Subtitler::OnSocketDisconnected()
    {
        OutputDebugStringW(L"Subtitler: Socket disconnected\n");
    }

    void Subtitler::OnSocketError(const hstring& errorString)
    {
        OutputDebugStringW((L"Subtitler: Socket error - " + errorString + L"\n").c_str());
    }

    void Subtitler::OnSocketProcessingStarted(const ProcessingStartedEvent& event)
    {
        OutputDebugStringW((L"Subtitler: Processing started at " + to_hstring(event.seek_time) + L" seconds\n").c_str());
        m_isProcessing = true;
        
        if (OnProcessingStarted)
        {
            OnProcessingStarted(event.seek_time);
        }
    }

    void Subtitler::OnSocketSubtitleReceived(const SubtitleEvent& event)
    {
        OutputDebugStringW((L"Subtitler: Received subtitle: " + event.text + L"\n").c_str());

        // Create subtitle segment with times in milliseconds
        SubtitleSegment segment;
        segment.startTime = static_cast<int64_t>(event.start * 1000);
        segment.endTime = static_cast<int64_t>(event.end * 1000);
        segment.nativeText = event.text;
        segment.translatedText = event.text_en;

        // Insert subtitle in time order
        auto insertPos = std::lower_bound(m_subtitles.begin(), m_subtitles.end(), segment,
            [](const SubtitleSegment& a, const SubtitleSegment& b) {
                return a.startTime < b.startTime;
            });
        m_subtitles.insert(insertPos, segment);

        // Update coverage range with this subtitle's end time
        AddOrExtendCoverageRange(segment.endTime);

        if (OnSubtitlesUpdated)
        {
            OnSubtitlesUpdated();
        }
    }

    void Subtitler::OnSocketTranscriptionCompleted(const CompletedEvent& event)
    {
        OutputDebugStringW((L"Subtitler: Transcription completed, language: " + event.language + L"\n").c_str());
        m_detectedLanguage = event.language;
        m_isProcessing = false;
        
        if (OnProcessingFinished)
        {
            OnProcessingFinished();
        }
    }

    void Subtitler::OnSocketAudioLoaded()
    {
        OutputDebugStringW(L"Subtitler: Audio loaded on server\n");
        m_audioLoaded = true;
    }

    bool Subtitler::IsPositionCovered(int64_t timeMs) const
    {
        for (const auto& range : m_coverageRanges)
        {
            if (timeMs >= range.start && timeMs <= range.end)
            {
                return true;
            }
        }
        return false;
    }

    void Subtitler::AddOrExtendCoverageRange(int64_t endTime)
    {
        if (m_coverageRanges.empty())
        {
            OutputDebugStringW(L"Subtitler: No coverage ranges exist, cannot extend\n");
            return;
        }

        // Find the last (most recent) coverage range and extend it
        auto& lastRange = m_coverageRanges.back();
        if (endTime > lastRange.end)
        {
            lastRange.end = endTime;
            OutputDebugStringW((L"Subtitler: Extended coverage range to " + to_hstring(endTime) + L" ms\n").c_str());

            // Check if we can merge with any adjacent ranges
            for (size_t i = m_coverageRanges.size() - 1; i > 0; --i)
            {
                auto& currentRange = m_coverageRanges[i - 1];
                auto& nextRange = m_coverageRanges[i];

                // If ranges overlap or are adjacent (within 100ms), merge them
                if (currentRange.end + 100 >= nextRange.start)
                {
                    currentRange.end = (std::max)(currentRange.end, nextRange.end);
                    m_coverageRanges.erase(m_coverageRanges.begin() + i);
                    OutputDebugStringW(L"Subtitler: Merged coverage ranges\n");
                }
            }
        }
    }

    void Subtitler::StartNewCoverageRange(int64_t startTime)
    {
        CoverageRange range;
        range.start = startTime;
        range.end = startTime;
        m_coverageRanges.push_back(range);
        OutputDebugStringW((L"Subtitler: Started new coverage range at " + to_hstring(startTime) + L" ms\n").c_str());
    }
}
