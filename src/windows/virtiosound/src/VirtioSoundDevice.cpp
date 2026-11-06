// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "VirtioSoundDevice.hpp"
#ifdef _WIN32
#include "windows/AudioSourceTrace.hpp"
#endif

namespace wsl::virtio_sound::native {
namespace {
    void TraceCapture(const wchar_t* stage, std::uint64_t value = 0, std::uint64_t extra = 0) noexcept
    {
#ifdef _WIN32
        windows::detail::TraceAudioSource(stage, value, extra);
#else
        (void)stage;
        (void)value;
        (void)extra;
#endif
    }
    constexpr std::uint32_t c_statusOk = 0x8000;
    constexpr std::uint32_t c_statusBadMessage = 0x8001;
    constexpr std::uint32_t c_statusNotSupported = 0x8002;
    constexpr std::uint32_t c_statusIoError = 0x8003;
    constexpr std::uint32_t c_requestPcmInfo = 0x0100;
    constexpr std::uint32_t c_requestPcmSetParams = 0x0101;
    constexpr std::uint32_t c_requestPcmPrepare = 0x0102;
    constexpr std::uint32_t c_requestPcmRelease = 0x0103;
    constexpr std::uint32_t c_requestPcmStart = 0x0104;
    constexpr std::uint32_t c_requestPcmStop = 0x0105;
    constexpr std::uint32_t c_captureStreamId = 0;
    constexpr std::uint32_t c_pcmFeatureXruns = 1U << 4;
    constexpr std::uint32_t c_supportedPcmFeatures = c_pcmFeatureXruns;
    constexpr std::uint64_t c_deviceFeatures = 1ULL << 32;     // VIRTIO_F_VERSION_1
    constexpr std::uint16_t c_virtqUsedFlagNoNotify = 1;       // VIRTQ_USED_F_NO_NOTIFY; EVENT_IDX is not advertised
    constexpr std::uint64_t c_supportedFormatMask = 1ULL << 5; // S16_LE
    constexpr std::uint32_t c_maxPcmPayloadBytes = 1024 * 1024 - 12;

    std::uint64_t ReadLe(const std::uint8_t* bytes, std::size_t offset, std::size_t count)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
        }
        return value;
    }

    void WriteLe(std::uint8_t* bytes, std::size_t offset, std::uint64_t value, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }

    std::uint64_t AddAddress(std::uint64_t address, std::uint64_t length)
    {
        if (std::numeric_limits<std::uint64_t>::max() - address < length)
        {
            throw std::runtime_error("Guest address overflow");
        }
        return address + length;
    }

    void Increment(std::uint64_t& value) noexcept
    {
        if (value != std::numeric_limits<std::uint64_t>::max())
        {
            ++value;
        }
    }

    int RateIndex(std::uint32_t sampleRate)
    {
        constexpr std::array<std::uint32_t, 14> c_rates{
            5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 384000};
        auto found = std::find(c_rates.begin(), c_rates.end(), sampleRate);
        return found == c_rates.end() ? -1 : static_cast<int>(found - c_rates.begin());
    }

} // namespace

class VirtioSoundDevice::Impl final
{
public:
    explicit Impl(GuestMemoryCallbacks guestMemory, InterruptCallbacks interruptCallbacks, std::shared_ptr<PcmSource> source, PcmFormat pcmFormat) :
        m_memory(guestMemory),
        m_interrupts(interruptCallbacks),
        m_pcmSource(std::move(source)),
        m_format(pcmFormat),
        m_rateIndex(RateIndex(pcmFormat.m_sampleRate)),
        m_frameBytes(static_cast<std::uint32_t>(pcmFormat.m_channels) * 2)
    {
        if (m_memory.m_read == nullptr || m_memory.m_write == nullptr)
        {
            throw std::invalid_argument("Guest-memory read and write callbacks are required");
        }
        if (m_interrupts.m_deliverMsi == nullptr)
        {
            throw std::invalid_argument("An MSI callback is required");
        }
        if (m_format.m_channels == 0 || m_rateIndex < 0)
        {
            throw std::invalid_argument("PCM format is not representable by virtio-sound");
        }
        if (!m_pcmSource && (m_format.m_channels != 2 || m_format.m_sampleRate != 48000))
        {
            throw std::invalid_argument("A PCM source is required for non-default formats");
        }

        WriteLe(m_pci.data(), 0, VirtioSoundDevice::c_vendorId, 2);
        WriteLe(m_pci.data(), 2, VirtioSoundDevice::c_deviceId, 2);
        WriteLe(m_pci.data(), 6, 0x10, 2);
        m_pci[8] = 1;
        m_pci[10] = 1;
        m_pci[11] = 4; // multimedia audio
        WriteLe(m_pci.data(), 0x2c, VirtioSoundDevice::c_vendorId, 2);
        WriteLe(m_pci.data(), 0x2e, 25, 2);
        m_pci[0x34] = 0x40;
        m_pci[0x3d] = m_interrupts.m_setIntx == nullptr ? 0 : 1;
        Capability(0x40, 0x50, 1, 0, 56, 16);
        Capability(0x50, 0x64, 2, 0x300, 16, 20);
        WriteLe(m_pci.data(), 0x60, 4, 4); // notify multiplier
        Capability(0x64, 0x74, 3, 0x100, 1, 16);
        Capability(0x74, 0x84, 4, 0x200, 12, 16);
        m_pci[0x84] = 0x11; // MSI-X
        WriteLe(m_pci.data(), 0x86, 4, 2);
        WriteLe(m_pci.data(), 0x88, 0x1000, 4);
        WriteLe(m_pci.data(), 0x8c, 0x1800, 4);
        for (int i = 0; i < 5; ++i)
        {
            WriteLe(m_msix.data(), static_cast<std::size_t>(i * 16 + 12), 1, 4);
        }
        Reset();
    }

    ~Impl() noexcept
    {
        if (m_pcmSource && m_streamState == StreamState::Running)
        {
            m_pcmSource->StopCapture();
        }
    }

    struct Queue
    {
        std::uint16_t m_size{VirtioSoundDevice::c_maxQueueSize};
        std::uint16_t m_vector{0xffff};
        std::uint16_t m_lastAvailable{};
        std::uint16_t m_observedAvailable{};
        std::uint16_t m_used{};
        bool m_enabled{};
        std::uint8_t m_addressPartsSet{};
        std::uint64_t m_descriptor{};
        std::uint64_t m_available{};
        std::uint64_t m_usedRing{};
    };

    struct Segment
    {
        std::uint64_t m_address{};
        std::size_t m_length{};
    };

    struct Chain
    {
        std::uint16_t m_head{};
        std::vector<std::uint8_t> m_request;
        bool m_hasReadable{};
        std::vector<Segment> m_writable;
        std::size_t m_capacity{};
    };

    enum class StreamState
    {
        Unconfigured,
        Configured,
        Prepared,
        Running,
        Stopped
    };

    GuestMemoryCallbacks m_memory;
    InterruptCallbacks m_interrupts;
    std::shared_ptr<PcmSource> m_pcmSource;
    PcmFormat m_format;
    int m_rateIndex{};
    std::uint32_t m_frameBytes{};
    // This portable core uses the standard lock types so its standalone CMake target stays WIL-free.
    mutable std::recursive_mutex m_gate;
    std::array<std::uint8_t, 256> m_pci{};
    std::array<std::uint8_t, 80> m_msix{};
    std::array<Queue, VirtioSoundDevice::c_queueCount> m_queues{};
    std::optional<Chain> m_pendingStartControl;
    // Retain the guest-owned RX chain after inspection until used-ring completion;
    // the guest must not modify a descriptor chain while ownership is with the device.
    std::optional<Chain> m_pendingRx;
    std::uint32_t m_deviceFeatureSelect{};
    std::uint32_t m_driverFeatureSelect{};
    std::uint64_t m_driverFeatures{};
    std::uint64_t m_pendingVectors{};
    std::uint64_t m_diagnosticResetCount{};
    std::uint64_t m_rxAvailableProcessed{};
    std::uint64_t m_rxUsedEntriesPublished{};
    std::uint64_t m_rxDataCompletions{};
    std::uint64_t m_rxStatusOnlyCompletions{};
    std::uint64_t m_notificationRequests{};
    std::uint64_t m_notificationFailures{};
    std::uint64_t m_interruptSignalsRequested{};
    std::uint64_t m_interruptApiRequests{};
    std::uint64_t m_interruptApiSuccesses{};
    std::uint64_t m_interruptApiFailures{};
    std::uint16_t m_queueSelect{};
    std::uint16_t m_configVector{0xffff};
    std::uint8_t m_status{};
    std::uint8_t m_isr{};
    bool m_intxAsserted{};
    bool m_pendingXrun{};
    std::uint32_t m_bufferBytes{};
    std::uint32_t m_periodBytes{};
    std::uint32_t m_negotiatedFeatures{};
    std::uint32_t m_frameCursor{};
    std::uint64_t m_pacingGeneration{};
    std::uint64_t m_observedPacingGeneration{};
    long double m_elapsedFrameCredit{};
    StreamState m_streamState{StreamState::Unconfigured};
    bool m_sourceFailureReported{};
    std::string m_lastFault;

    void Capability(int at, int next, int kind, std::uint32_t offset, std::uint32_t length, int size)
    {
        m_pci[at] = 9;
        m_pci[at + 1] = static_cast<std::uint8_t>(next);
        m_pci[at + 2] = static_cast<std::uint8_t>(size);
        m_pci[at + 3] = static_cast<std::uint8_t>(kind);
        WriteLe(m_pci.data(), static_cast<std::size_t>(at + 8), offset, 4);
        WriteLe(m_pci.data(), static_cast<std::size_t>(at + 12), length, 4);
    }

    static void CheckPciAccess(std::uint32_t offset, std::uint8_t width)
    {
        if ((width != 1 && width != 2 && width != 4) || offset > 256U - width || offset % width != 0)
        {
            throw std::out_of_range("PCI access must be aligned byte/word/dword");
        }
    }

    static void CheckBarAccess(std::uint64_t offset, std::size_t length)
    {
        if ((length != 1 && length != 2 && length != 4 && length != 8) || offset >= VirtioSoundDevice::c_barSize ||
            length > VirtioSoundDevice::c_barSize - offset || offset % length != 0)
        {
            throw std::out_of_range("BAR access must be aligned and bounded");
        }
    }

    std::uint32_t ReadConfig(std::uint32_t offset, std::uint8_t width) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        CheckPciAccess(offset, width);
        return static_cast<std::uint32_t>(ReadLe(m_pci.data(), offset, width));
    }

    void WriteConfig(std::uint32_t offset, std::uint32_t value, std::uint8_t width)
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        CheckPciAccess(offset, width);
        std::uint32_t aligned = offset & ~3U;
        std::uint32_t shift = (offset & 3U) * 8U;
        std::uint64_t lane = (width == 4 ? 0xffffffffULL : ((1ULL << (width * 8U)) - 1ULL)) << shift;
        std::uint32_t merged =
            static_cast<std::uint32_t>((ReadLe(m_pci.data(), aligned, 4) & ~lane) | ((static_cast<std::uint64_t>(value) << shift) & lane));
        if (aligned == 4)
        {
            WriteLe(m_pci.data(), 4, merged & 0x407, 2);
            UpdateIntx();
        }
        else if (aligned == 0x10)
        {
            WriteLe(m_pci.data(), 0x10, merged == std::numeric_limits<std::uint32_t>::max() ? 0xffffe000U : merged & 0xffffe000U, 4);
        }
        else if (aligned == 0x84)
        {
            WriteLe(m_pci.data(), 0x86, ((merged >> 16) & 0xc000U) | 4U, 2);
            FlushPending();
        }
    }

    void ReadBar(std::uint64_t offset, std::uint8_t* destination, std::size_t length)
    {
        if (destination == nullptr)
        {
            throw std::invalid_argument("BAR read destination is null");
        }
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        CheckBarAccess(offset, length);
        std::fill(destination, destination + length, static_cast<std::uint8_t>(0));
        if (offset < 56 && offset + length <= 56)
        {
            auto common = Common();
            std::copy_n(common.data() + static_cast<std::size_t>(offset), length, destination);
        }
        else if (offset == 0x100 && length == 1)
        {
            destination[0] = m_isr;
            m_isr = 0;
            UpdateIntx();
        }
        else if (offset >= 0x200 && offset + length <= 0x20c)
        {
            std::array<std::uint8_t, 12> config{};
            WriteLe(config.data(), 4, 1, 4);
            std::copy_n(config.data() + static_cast<std::size_t>(offset - 0x200), length, destination);
        }
        else if (offset >= 0x1000 && offset + length <= 0x1050)
        {
            std::copy_n(m_msix.data() + static_cast<std::size_t>(offset - 0x1000), length, destination);
        }
        else if (offset >= 0x1800 && offset + length <= 0x1808)
        {
            std::array<std::uint8_t, 8> pba{};
            WriteLe(pba.data(), 0, m_pendingVectors, 8);
            std::copy_n(pba.data() + static_cast<std::size_t>(offset - 0x1800), length, destination);
        }
    }

    void WriteBar(std::uint64_t offset, const std::uint8_t* source, std::size_t length)
    {
        if (source == nullptr)
        {
            throw std::invalid_argument("BAR write source is null");
        }
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        CheckBarAccess(offset, length);
        std::uint64_t value = ReadLe(source, 0, length);
        if (offset >= 0x1000 && offset + length <= 0x1050)
        {
            std::copy_n(source, length, m_msix.data() + static_cast<std::size_t>(offset - 0x1000));
            FlushPending();
            return;
        }
        if (offset >= 0x300 && offset < 0x310 && length == 2 && ((offset - 0x300) % 4) == 0)
        {
            std::uint32_t index = static_cast<std::uint32_t>((offset - 0x300) / 4);
            Increment(m_notificationRequests);
            if (value != index)
            {
                Increment(m_notificationFailures);
                Fault("Notification queue index mismatch");
                return;
            }
            if (index == 0)
            {
                ProcessControlQueue(); // RX is paced by PumpCapture.
            }
            else if (index == 1)
            {
                ProcessEventQueue();
            }
            return;
        }

        Queue* queue = m_queueSelect < c_queueCount ? &m_queues[m_queueSelect] : nullptr;
        switch (offset)
        {
        case 0:
            if (length == 4)
            {
                m_deviceFeatureSelect = static_cast<std::uint32_t>(value);
            }
            break;
        case 8:
            if (length == 4)
            {
                m_driverFeatureSelect = static_cast<std::uint32_t>(value);
            }
            break;
        case 12:
            if (length == 4 && (m_status & 8) == 0 && m_driverFeatureSelect < 2)
            {
                int shift = static_cast<int>(m_driverFeatureSelect) * 32;
                m_driverFeatures = (m_driverFeatures & ~(0xffffffffULL << shift)) | (value << shift);
            }
            break;
        case 16:
            if (length == 2)
            {
                m_configVector = value < 5 ? static_cast<std::uint16_t>(value) : 0xffff;
            }
            break;
        case 20:
            if (length == 1)
            {
                SetStatus(static_cast<std::uint8_t>(value));
            }
            break;
        case 22:
            if (length == 2)
            {
                m_queueSelect = static_cast<std::uint16_t>(value);
            }
            break;
        case 24:
            if (length == 2 && queue != nullptr && !queue->m_enabled)
            {
                queue->m_size = static_cast<std::uint16_t>(value);
            }
            break;
        case 26:
            if (length == 2 && queue != nullptr)
            {
                queue->m_vector = value < 5 ? static_cast<std::uint16_t>(value) : 0xffff;
            }
            break;
        case 28:
            if (length == 2 && queue != nullptr && value == 1 && !queue->m_enabled)
            {
                if (queue->m_size == 0 || queue->m_size > c_maxQueueSize || (queue->m_size & (queue->m_size - 1)) != 0 ||
                    queue->m_addressPartsSet != 0x3f || (queue->m_descriptor & 15) != 0 || (queue->m_available & 1) != 0 ||
                    (queue->m_usedRing & 3) != 0)
                {
                    Fault("Invalid virtqueue configuration");
                }
                else
                {
                    // RX is consumed by PumpCapture, not by queue kicks. Advise the
                    // driver that it need not synchronously notify us after adding
                    // RX buffers. Publish this device-owned used-ring metadata only
                    // after validating the queue, and do not expose an enabled RX
                    // queue if guest memory rejects the write.
                    if (queue == &m_queues[3])
                    {
                        std::array<std::uint8_t, 2> flags{};
                        WriteLe(flags.data(), 0, c_virtqUsedFlagNoNotify, flags.size());
                        try
                        {
                            WriteGuest(queue->m_usedRing, flags.data(), flags.size());
                        }
                        catch (...)
                        {
                            Fault("Failed to initialize RX used-ring notification flags");
                            break;
                        }
                    }
                    queue->m_enabled = true;
                }
            }
            break;
        case 32:
        case 36:
        case 40:
        case 44:
        case 48:
        case 52:
            if (queue != nullptr && !queue->m_enabled && (length == 4 || (length == 8 && offset % 8 == 0)))
            {
                int part = static_cast<int>(offset - 32) / 8;
                int partShift = static_cast<int>(offset % 8) * 8;
                std::uint64_t old = part == 0 ? queue->m_descriptor : part == 1 ? queue->m_available : queue->m_usedRing;
                std::uint64_t merged = length == 8 ? value : (old & ~(0xffffffffULL << partShift)) | (value << partShift);
                if (part == 0)
                {
                    queue->m_descriptor = merged;
                }
                else if (part == 1)
                {
                    queue->m_available = merged;
                }
                else
                {
                    queue->m_usedRing = merged;
                }
                queue->m_addressPartsSet |=
                    static_cast<std::uint8_t>(length == 8 ? (3 << (part * 2)) : (1 << (part * 2 + (partShift == 0 ? 0 : 1))));
            }
            break;
        default:
            break;
        }
    }

    std::array<std::uint8_t, 56> Common() const
    {
        std::array<std::uint8_t, 56> common{};
        WriteLe(common.data(), 0, m_deviceFeatureSelect, 4);
        WriteLe(common.data(), 4, m_deviceFeatureSelect < 2 ? (c_deviceFeatures >> static_cast<int>(m_deviceFeatureSelect * 32)) & 0xffffffffULL : 0, 4);
        WriteLe(common.data(), 8, m_driverFeatureSelect, 4);
        WriteLe(common.data(), 12, m_driverFeatureSelect < 2 ? (m_driverFeatures >> static_cast<int>(m_driverFeatureSelect * 32)) & 0xffffffffULL : 0, 4);
        WriteLe(common.data(), 16, m_configVector, 2);
        WriteLe(common.data(), 18, c_queueCount, 2);
        common[20] = m_status;
        WriteLe(common.data(), 22, m_queueSelect, 2);
        if (m_queueSelect < c_queueCount)
        {
            const Queue& queue = m_queues[m_queueSelect];
            WriteLe(common.data(), 24, queue.m_size, 2);
            WriteLe(common.data(), 26, queue.m_vector, 2);
            WriteLe(common.data(), 28, queue.m_enabled ? 1 : 0, 2);
            WriteLe(common.data(), 30, m_queueSelect, 2);
            WriteLe(common.data(), 32, queue.m_descriptor, 8);
            WriteLe(common.data(), 40, queue.m_available, 8);
            WriteLe(common.data(), 48, queue.m_usedRing, 8);
        }
        return common;
    }

    void SetStatus(std::uint8_t value)
    {
        if (value == 0)
        {
            Reset();
            return;
        }
        if ((value & m_status) != m_status)
        {
            Fault("Device status requires reset before clearing bits");
            return;
        }
        m_status = value;
        if ((m_status & 8) != 0 && (m_driverFeatures != c_deviceFeatures || (m_status & 3) != 3))
        {
            m_status &= 0xf7;
        }
        if ((m_status & 4) != 0 && (m_status & 11) != 11)
        {
            Fault("DRIVER_OK before feature negotiation");
        }
    }

    bool Ready() const noexcept
    {
        return (m_status & 0xcf) == 15;
    }

    void Reset()
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        m_pendingStartControl.reset();
        m_pendingRx.reset();
        m_status = 0;
        m_isr = 0;
        m_driverFeatures = 0;
        m_deviceFeatureSelect = 0;
        m_driverFeatureSelect = 0;
        m_queueSelect = 0;
        m_configVector = 0xffff;
        m_pendingVectors = 0;
        m_lastFault.clear();
        for (Queue& queue : m_queues)
        {
            queue = Queue{};
        }
        m_pendingXrun = false;
        m_streamState = StreamState::Unconfigured;
        m_bufferBytes = 0;
        m_periodBytes = 0;
        m_negotiatedFeatures = 0;
        m_frameCursor = 0;
        m_elapsedFrameCredit = 0;
        StopSource();
        Increment(m_pacingGeneration);
        Increment(m_diagnosticResetCount);
        UpdateIntx();
    }

    void Fault(const std::string& message)
    {
        TraceCapture(L"DeviceFault", m_status, static_cast<std::uint64_t>(m_streamState));
        m_pendingStartControl.reset();
        m_pendingRx.reset();
        if (m_streamState == StreamState::Running)
        {
            StopSource();
            m_streamState = StreamState::Stopped;
            m_elapsedFrameCredit = 0;
            Increment(m_pacingGeneration);
        }
        m_lastFault = message;
        m_status |= 64;
        RaiseInterrupt(m_configVector, 2);
    }

    void StopSource() noexcept
    {
        if (m_pcmSource)
        {
            m_pcmSource->StopCapture();
        }
        m_sourceFailureReported = false;
    }

    SourceReadiness CurrentSourceReadiness() const noexcept
    {
        if (!m_pcmSource)
        {
            return SourceReadiness::Ready;
        }
        const SourceReadiness readiness = m_pcmSource->CaptureReadiness();
        switch (readiness)
        {
        case SourceReadiness::Pending:
        case SourceReadiness::Ready:
        case SourceReadiness::Failed:
            return readiness;
        }
        return SourceReadiness::Failed;
    }

    std::vector<std::uint8_t> HandleControl(const std::vector<std::uint8_t>& request)
    {
        if (request.size() < 4)
        {
            return StatusResponse(c_statusBadMessage);
        }
        try
        {
            std::uint32_t code = static_cast<std::uint32_t>(ReadLe(request.data(), 0, 4));
            switch (code)
            {
            case c_requestPcmInfo:
                return HandlePcmInfo(request);
            case c_requestPcmSetParams:
                return HandleSetParams(request);
            case c_requestPcmPrepare:
                return HandleSimpleCommand(request, StreamState::Prepared);
            case c_requestPcmRelease:
                return HandleSimpleCommand(request, StreamState::Configured);
            case c_requestPcmStart:
                return HandleSimpleCommand(request, StreamState::Running);
            case c_requestPcmStop:
                return HandleSimpleCommand(request, StreamState::Stopped);
            default:
                return StatusResponse(c_statusNotSupported);
            }
        }
        catch (...)
        {
            return StatusResponse(c_statusIoError);
        }
    }

    static std::vector<std::uint8_t> StatusResponse(std::uint32_t code)
    {
        std::vector<std::uint8_t> response(4);
        WriteLe(response.data(), 0, code, 4);
        return response;
    }

    std::vector<std::uint8_t> HandlePcmInfo(const std::vector<std::uint8_t>& request) const
    {
        if (request.size() != 16)
        {
            return StatusResponse(c_statusBadMessage);
        }
        std::uint32_t startId = static_cast<std::uint32_t>(ReadLe(request.data(), 4, 4));
        std::uint32_t count = static_cast<std::uint32_t>(ReadLe(request.data(), 8, 4));
        std::uint32_t itemSize = static_cast<std::uint32_t>(ReadLe(request.data(), 12, 4));
        std::uint64_t endId = static_cast<std::uint64_t>(startId) + count;
        if (itemSize != 32 || startId > 1 || count > 1 || endId > 1)
        {
            return StatusResponse(c_statusBadMessage);
        }
        if (count == 0)
        {
            return StatusResponse(c_statusOk);
        }
        std::vector<std::uint8_t> response(36, 0);
        WriteLe(response.data(), 0, c_statusOk, 4);
        WriteLe(response.data(), 4, 0, 4); // hda_fn_nid
        WriteLe(response.data(), 8, c_supportedPcmFeatures, 4);
        WriteLe(response.data(), 12, c_supportedFormatMask, 8);
        WriteLe(response.data(), 20, 1ULL << m_rateIndex, 8);
        response[28] = 1; // input/capture
        response[29] = m_format.m_channels;
        response[30] = m_format.m_channels;
        return response;
    }

    std::vector<std::uint8_t> HandleSetParams(const std::vector<std::uint8_t>& request)
    {
        if (request.size() != 24)
        {
            return StatusResponse(c_statusBadMessage);
        }
        std::uint32_t streamId = static_cast<std::uint32_t>(ReadLe(request.data(), 4, 4));
        std::uint32_t newBufferBytes = static_cast<std::uint32_t>(ReadLe(request.data(), 8, 4));
        std::uint32_t newPeriodBytes = static_cast<std::uint32_t>(ReadLe(request.data(), 12, 4));
        std::uint32_t features = static_cast<std::uint32_t>(ReadLe(request.data(), 16, 4));
        std::uint8_t channels = request[20];
        std::uint8_t sampleFormat = request[21];
        std::uint8_t rate = request[22];
        std::uint8_t padding = request[23];
        if (streamId != c_captureStreamId || padding != 0)
        {
            return StatusResponse(c_statusBadMessage);
        }
        if ((features & ~c_supportedPcmFeatures) != 0 || channels != m_format.m_channels || sampleFormat != 5 || rate != m_rateIndex)
        {
            return StatusResponse(c_statusNotSupported);
        }
        if (newBufferBytes == 0 || newPeriodBytes == 0 || newPeriodBytes > newBufferBytes ||
            newPeriodBytes > c_maxPcmPayloadBytes || newBufferBytes % m_frameBytes != 0 || newPeriodBytes % m_frameBytes != 0)
        {
            return StatusResponse(c_statusBadMessage);
        }
        if (m_streamState != StreamState::Unconfigured && m_streamState != StreamState::Configured && m_streamState != StreamState::Prepared)
        {
            return StatusResponse(c_statusBadMessage);
        }
        if (m_pcmSource && m_pcmSource->AvailableFrames().has_value())
        {
            // A clocked source must drain at least one pump interval per tick,
            // in whole guest periods, while retaining one period of headroom.
            const auto bufferFrames = newBufferBytes / m_frameBytes;
            const auto periodFrames = newPeriodBytes / m_frameBytes;
            const auto burstFrames = (std::min)(c_maxTransferBytes / m_frameBytes, bufferFrames - periodFrames);
            const auto wholePeriodFrames = (burstFrames / periodFrames) * periodFrames;
            const auto intervalFrames =
                (static_cast<std::uint64_t>(m_format.m_sampleRate) * c_capturePumpIntervalMs + 999) / 1000;
            if (wholePeriodFrames < intervalFrames)
            {
                return StatusResponse(c_statusNotSupported);
            }
        }
        StopSource();
        m_bufferBytes = newBufferBytes;
        m_periodBytes = newPeriodBytes;
        m_negotiatedFeatures = features;
        m_frameCursor = 0;
        m_streamState = StreamState::Configured;
        Increment(m_pacingGeneration);
        return StatusResponse(c_statusOk);
    }

    std::vector<std::uint8_t> HandleSimpleCommand(const std::vector<std::uint8_t>& request, StreamState command)
    {
        if (request.size() != 8 || ReadLe(request.data(), 4, 4) != c_captureStreamId)
        {
            return StatusResponse(c_statusBadMessage);
        }
        switch (command)
        {
        case StreamState::Prepared:
            if (m_streamState != StreamState::Configured && m_streamState != StreamState::Prepared)
            {
                return StatusResponse(c_statusBadMessage);
            }
            StopSource();
            m_streamState = StreamState::Prepared;
            m_frameCursor = 0;
            Increment(m_pacingGeneration);
            return StatusResponse(c_statusOk);
        case StreamState::Running:
            if (m_streamState != StreamState::Prepared && m_streamState != StreamState::Stopped)
            {
                return StatusResponse(c_statusBadMessage);
            }
            m_sourceFailureReported = false;
            if (m_pcmSource && !m_pcmSource->StartCapture(m_periodBytes / m_frameBytes))
            {
                StopSource();
                return StatusResponse(c_statusIoError);
            }
            m_streamState = StreamState::Running;
            Increment(m_pacingGeneration);
            return StatusResponse(c_statusOk);
        case StreamState::Stopped:
            if (m_streamState != StreamState::Running)
            {
                return StatusResponse(c_statusBadMessage);
            }
            StopSource();
            m_streamState = StreamState::Stopped;
            Increment(m_pacingGeneration);
            return StatusResponse(c_statusOk);
        case StreamState::Configured:
            if (m_streamState != StreamState::Prepared && m_streamState != StreamState::Stopped)
            {
                return StatusResponse(c_statusBadMessage);
            }
            StopSource();
            m_streamState = StreamState::Configured;
            m_frameCursor = 0;
            Increment(m_pacingGeneration);
            return StatusResponse(c_statusOk);
        default:
            return StatusResponse(c_statusNotSupported);
        }
    }

    bool IsRunning() const noexcept
    {
        return m_streamState == StreamState::Running;
    }
    bool XRunEventsEnabled() const noexcept
    {
        return (m_negotiatedFeatures & c_pcmFeatureXruns) != 0;
    }

    std::vector<std::uint8_t> ReadGuest(std::uint64_t address, std::size_t length) const
    {
        AddAddress(address, length);
        std::vector<std::uint8_t> bytes(length);
        if (length != 0 && !m_memory.m_read(m_memory.m_context, address, bytes.data(), length))
        {
            throw std::runtime_error("Guest memory read failed");
        }
        return bytes;
    }

    void WriteGuest(std::uint64_t address, const std::uint8_t* source, std::size_t length) const
    {
        AddAddress(address, length);
        if (length != 0 && !m_memory.m_write(m_memory.m_context, address, source, length))
        {
            throw std::runtime_error("Guest memory write failed");
        }
    }

    Chain* Peek(Queue& queue, Chain& chain)
    {
        auto availHeader = ReadGuest(queue.m_available, 4);
        std::uint16_t available = static_cast<std::uint16_t>(ReadLe(availHeader.data(), 2, 2));
        queue.m_observedAvailable = available;
        std::uint16_t pending = static_cast<std::uint16_t>(available - queue.m_lastAvailable);
        if (pending == 0)
        {
            return nullptr;
        }
        if (pending > queue.m_size)
        {
            throw std::runtime_error("Available ring overrun");
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto headBytes = ReadGuest(AddAddress(queue.m_available, 4ULL + 2ULL * (queue.m_lastAvailable % queue.m_size)), 2);
        chain = Chain{};
        chain.m_head = static_cast<std::uint16_t>(ReadLe(headBytes.data(), 0, 2));
        bool writableSeen = false;
        std::array<bool, c_maxQueueSize> visited{};
        std::uint16_t index = chain.m_head;
        for (std::uint32_t n = 0; n < queue.m_size; ++n)
        {
            if (index >= queue.m_size || visited[index])
            {
                throw std::runtime_error("Invalid/cyclic descriptor chain");
            }
            visited[index] = true;
            auto descriptor = ReadGuest(AddAddress(queue.m_descriptor, 16ULL * index), 16);
            std::uint64_t address = ReadLe(descriptor.data(), 0, 8);
            std::uint32_t length = static_cast<std::uint32_t>(ReadLe(descriptor.data(), 8, 4));
            std::uint16_t flags = static_cast<std::uint16_t>(ReadLe(descriptor.data(), 12, 2));
            if ((flags & ~3U) != 0)
            {
                throw std::runtime_error("Unnegotiated descriptor flags");
            }
            if (length > c_maxTransferBytes || static_cast<std::uint64_t>(length) + chain.m_request.size() + chain.m_capacity > c_maxTransferBytes)
            {
                throw std::runtime_error("Descriptor chain exceeds transfer bound");
            }
            AddAddress(address, length);
            if ((flags & 2U) != 0)
            {
                writableSeen = true;
                chain.m_capacity += length;
                chain.m_writable.push_back(Segment{address, length});
            }
            else
            {
                if (writableSeen)
                {
                    throw std::runtime_error("Readable descriptor after writable descriptor");
                }
                chain.m_hasReadable = true;
                auto request = ReadGuest(address, length);
                chain.m_request.insert(chain.m_request.end(), request.begin(), request.end());
            }
            if ((flags & 1U) == 0)
            {
                return &chain;
            }
            index = static_cast<std::uint16_t>(ReadLe(descriptor.data(), 14, 2));
        }
        throw std::runtime_error("Descriptor chain is too long");
    }

    Chain* PeekRx(Chain& chain)
    {
        if (m_pendingRx.has_value())
        {
            chain = *m_pendingRx;
            return &chain;
        }
        return Peek(m_queues[3], chain);
    }

    void Scatter(const Chain& chain, std::size_t offset, const std::uint8_t* value, std::size_t length)
    {
        if (offset > chain.m_capacity || length > chain.m_capacity - offset)
        {
            throw std::runtime_error("Response overrun");
        }
        std::size_t sourceOffset = 0;
        for (const Segment& segment : chain.m_writable)
        {
            if (offset >= segment.m_length)
            {
                offset -= segment.m_length;
                continue;
            }
            std::size_t count = std::min(segment.m_length - offset, length - sourceOffset);
            if (count != 0)
            {
                WriteGuest(AddAddress(segment.m_address, offset), value + sourceOffset, count);
                sourceOffset += count;
            }
            offset = 0;
            if (sourceOffset == length)
            {
                return;
            }
        }
    }

    void Complete(Queue& queue, const Chain& chain, std::uint32_t usedLength, bool rxDataCompletion = false, bool rxStatusOnlyCompletion = false)
    {
        std::array<std::uint8_t, 8> entry{};
        WriteLe(entry.data(), 0, chain.m_head, 4);
        WriteLe(entry.data(), 4, usedLength, 4);
        WriteGuest(AddAddress(queue.m_usedRing, 4ULL + 8ULL * (queue.m_used % queue.m_size)), entry.data(), entry.size());
        std::atomic_thread_fence(std::memory_order_seq_cst);
        queue.m_used = static_cast<std::uint16_t>(queue.m_used + 1);
        queue.m_lastAvailable = static_cast<std::uint16_t>(queue.m_lastAvailable + 1);
        bool isRx = &queue == &m_queues[3];
        if (isRx)
        {
            Increment(m_rxAvailableProcessed);
        }
        std::array<std::uint8_t, 2> index{};
        WriteLe(index.data(), 0, queue.m_used, 2);
        WriteGuest(AddAddress(queue.m_usedRing, 2), index.data(), index.size());
        if (isRx)
        {
            m_pendingRx.reset();
            Increment(m_rxUsedEntriesPublished);
            if (rxDataCompletion)
            {
                Increment(m_rxDataCompletions);
            }
            if (rxStatusOnlyCompletion)
            {
                Increment(m_rxStatusOnlyCompletions);
            }
        }
        auto availableFlags = ReadGuest(queue.m_available, 2);
        if ((ReadLe(availableFlags.data(), 0, 2) & 1) == 0)
        {
            RaiseInterrupt(queue.m_vector, 1);
        }
    }

    bool ServicePendingStartControl()
    {
        if (!m_pendingStartControl.has_value())
        {
            return true;
        }
        const SourceReadiness readiness = CurrentSourceReadiness();
        if (readiness == SourceReadiness::Pending)
        {
            return false;
        }

        // Clear the saved guest chain before touching guest memory. If a memory
        // callback fails, Fault() cannot later retry writes through stale DMA.
        Chain chain = std::move(*m_pendingStartControl);
        m_pendingStartControl.reset();
        const std::uint32_t result = readiness == SourceReadiness::Ready ? c_statusOk : c_statusIoError;
        if (result != c_statusOk)
        {
            StopSource();
            m_streamState = StreamState::Prepared;
            m_elapsedFrameCredit = 0;
            Increment(m_pacingGeneration);
        }
        const std::vector<std::uint8_t> response = StatusResponse(result);
        Scatter(chain, 0, response.data(), response.size());
        Complete(m_queues[0], chain, static_cast<std::uint32_t>(response.size()));
        TraceCapture(L"StartControlCompleted", result, readiness == SourceReadiness::Ready ? 1U : 0U);
        return true;
    }

    int ProcessControlQueue()
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        if (!Ready() || !m_queues[0].m_enabled)
        {
            return 0;
        }
        int count = 0;
        try
        {
            if (m_pendingStartControl.has_value())
            {
                if (!ServicePendingStartControl())
                {
                    return 0;
                }
                ++count;
            }
            for (; count < c_maxQueueSize; ++count)
            {
                Chain chain;
                if (Peek(m_queues[0], chain) == nullptr)
                {
                    break;
                }
                if (chain.m_capacity < 4)
                {
                    Complete(m_queues[0], chain, 0);
                    continue;
                }
                std::vector<std::uint8_t> response;
                try
                {
                    response = HandleControl(chain.m_request);
                }
                catch (...)
                {
                    response = StatusResponse(c_statusIoError);
                }
                if (response.size() < 4)
                {
                    response = StatusResponse(c_statusIoError);
                }
                if (response.size() > chain.m_capacity)
                {
                    response = StatusResponse(c_statusBadMessage);
                }
                const bool isSuccessfulStart = chain.m_request.size() >= 4 && ReadLe(chain.m_request.data(), 0, 4) == c_requestPcmStart &&
                                               ReadLe(response.data(), 0, 4) == c_statusOk;
                if (isSuccessfulStart)
                {
                    const SourceReadiness readiness = CurrentSourceReadiness();
                    if (readiness == SourceReadiness::Pending)
                    {
                        m_pendingStartControl.emplace(std::move(chain));
                        TraceCapture(L"StartControlPending", m_periodBytes / m_frameBytes);
                        break;
                    }
                    if (readiness == SourceReadiness::Failed)
                    {
                        StopSource();
                        m_streamState = StreamState::Prepared;
                        m_elapsedFrameCredit = 0;
                        Increment(m_pacingGeneration);
                        response = StatusResponse(c_statusIoError);
                    }
                }
                if (chain.m_request.size() >= 4 && ReadLe(chain.m_request.data(), 0, 4) == c_requestPcmRelease &&
                    ReadLe(response.data(), 0, 4) == c_statusOk)
                {
                    DrainCapture(); // RELEASE returns outstanding RX buffers before its own response.
                }
                Scatter(chain, 0, response.data(), response.size());
                Complete(m_queues[0], chain, static_cast<std::uint32_t>(response.size()));
            }
        }
        catch (const std::exception& error)
        {
            Fault(error.what());
        }
        return count;
    }

    bool GenerateFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount)
    {
        if (byteCount != static_cast<std::size_t>(frames) * m_frameBytes || byteCount > c_maxPcmPayloadBytes)
        {
            return false;
        }
        if (m_pcmSource)
        {
            return m_pcmSource->ReadFrames(frames, destination, byteCount);
        }
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            int ramp = static_cast<int>(m_frameCursor & 0x7fffU) - 16384;
            auto left = static_cast<std::int16_t>(ramp);
            auto right = static_cast<std::int16_t>(-ramp);
            std::size_t frameOffset = static_cast<std::size_t>(i) * 4;
            WriteLe(destination, frameOffset, static_cast<std::uint16_t>(left), 2);
            WriteLe(destination, frameOffset + 2, static_cast<std::uint16_t>(right), 2);
            m_frameCursor = (m_frameCursor + 1U) & 0x7fffU;
        }
        return true;
    }

    void CaptureError(const Chain& chain, std::uint32_t statusCode, std::uint32_t origin)
    {
        TraceCapture(L"CaptureStatusError", statusCode, origin);
        std::array<std::uint8_t, 8> tail{};
        WriteLe(tail.data(), 0, statusCode, 4);
        Scatter(chain, chain.m_capacity - 8, tail.data(), tail.size());
        Complete(m_queues[3], chain, 8, false, true);
    }

    int PumpCapture(int frameBudget)
    {
        if (frameBudget < 0 || frameBudget > static_cast<int>(c_maxTransferBytes / m_frameBytes))
        {
            throw std::out_of_range("Capture frame budget is outside the bounded transfer size");
        }
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        if (!Ready() || !IsRunning())
        {
            return 0;
        }
        if (m_pendingStartControl.has_value())
        {
            m_elapsedFrameCredit = 0;
            return 0;
        }
        const SourceReadiness readiness = CurrentSourceReadiness();
        if (readiness == SourceReadiness::Pending)
        {
            m_elapsedFrameCredit = 0;
            m_sourceFailureReported = false;
            return 0;
        }
        if (readiness == SourceReadiness::Failed)
        {
            m_elapsedFrameCredit = 0;
            if (!m_sourceFailureReported)
            {
                m_sourceFailureReported = true;
                LatchXrun();
            }
            if (!m_queues[3].m_enabled)
            {
                return 0;
            }
            try
            {
                for (int n = 0; n < c_maxQueueSize; ++n)
                {
                    Chain chain;
                    if (PeekRx(chain) == nullptr)
                    {
                        break;
                    }
                    if (chain.m_capacity < 8)
                    {
                        throw std::runtime_error("Capture response has no status buffer");
                    }
                    const std::size_t payloadLength = chain.m_capacity - 8;
                    if (chain.m_request.size() != 4 || ReadLe(chain.m_request.data(), 0, 4) != c_captureStreamId ||
                        payloadLength == 0 || payloadLength % m_frameBytes != 0)
                    {
                        CaptureError(chain, c_statusBadMessage, 1);
                        continue;
                    }
                    CaptureError(chain, c_statusIoError, 2);
                }
            }
            catch (const std::exception& error)
            {
                Fault(error.what());
            }
            return 0;
        }
        m_sourceFailureReported = false;
        if (!m_queues[3].m_enabled)
        {
            return 0;
        }
        if (m_pcmSource)
        {
            const std::optional<std::uint32_t> availableFrames = m_pcmSource->AvailableFrames();
            if (availableFrames.has_value())
            {
                const std::uint32_t maximumFrames = VirtioSoundDevice::c_maxTransferBytes / m_frameBytes;
                const std::uint32_t availableBudget = (std::min)(*availableFrames, maximumFrames);
                frameBudget = (std::min)(frameBudget, static_cast<int>(availableBudget));
            }
        }
        int frames = 0;
        try
        {
            for (int n = 0; n < c_maxQueueSize; ++n)
            {
                Chain chain;
                if (PeekRx(chain) == nullptr)
                {
                    break;
                }
                if (chain.m_capacity < 8)
                {
                    throw std::runtime_error("Capture response has no status buffer");
                }
                std::size_t payloadLength = chain.m_capacity - 8;
                if (chain.m_request.size() != 4 || ReadLe(chain.m_request.data(), 0, 4) != c_captureStreamId ||
                    payloadLength == 0 || payloadLength % m_frameBytes != 0)
                {
                    CaptureError(chain, c_statusBadMessage, 3);
                    continue;
                }
                std::uint32_t needed = static_cast<std::uint32_t>(payloadLength / m_frameBytes);
                if (needed > static_cast<std::uint32_t>(frameBudget - frames))
                {
                    m_pendingRx = chain;
                    break;
                }
                std::vector<std::uint8_t> payload(payloadLength);
                bool generated = false;
                try
                {
                    generated = GenerateFrames(needed, payload.data(), payload.size());
                }
                catch (...)
                {
                    generated = false;
                }
                if (!generated)
                {
                    TraceCapture(L"GenerateFramesFailed", needed, payloadLength);
                    CaptureError(chain, c_statusIoError, 4);
                    LatchXrun();
                    continue;
                }
                Scatter(chain, 0, payload.data(), payload.size());
                std::array<std::uint8_t, 8> tail{};
                WriteLe(tail.data(), 0, c_statusOk, 4); // latency is not supplied by this source API
                Scatter(chain, payloadLength, tail.data(), tail.size());
                Complete(m_queues[3], chain, static_cast<std::uint32_t>(chain.m_capacity), true, false);
                frames += static_cast<int>(needed);
            }
        }
        catch (const std::exception& error)
        {
            Fault(error.what());
        }
        return frames;
    }

    int PumpCaptureElapsed(std::uint64_t elapsedNanoseconds)
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        if (m_pendingStartControl.has_value())
        {
            try
            {
                if (!ServicePendingStartControl())
                {
                    m_elapsedFrameCredit = 0;
                    return 0;
                }
            }
            catch (const std::exception& error)
            {
                Fault(error.what());
                return 0;
            }
            catch (...)
            {
                Fault("Pending START completion failed");
                return 0;
            }
        }
        const bool pacingGenerationChanged = m_observedPacingGeneration != m_pacingGeneration;
        if (pacingGenerationChanged)
        {
            m_observedPacingGeneration = m_pacingGeneration;
            m_elapsedFrameCredit = 0;
        }
        if (!Ready() || !IsRunning())
        {
            m_elapsedFrameCredit = 0;
            return 0;
        }
        const SourceReadiness readiness = CurrentSourceReadiness();
        if (readiness == SourceReadiness::Pending)
        {
            m_elapsedFrameCredit = 0;
            return 0;
        }
        if (readiness == SourceReadiness::Failed)
        {
            m_elapsedFrameCredit = 0;
            if (pacingGenerationChanged)
            {
                return 0;
            }
            return PumpCapture(0);
        }
        if (!m_queues[3].m_enabled)
        {
            m_elapsedFrameCredit = 0;
            return 0;
        }

        if (m_pcmSource)
        {
            const std::optional<std::uint32_t> availableFrames = m_pcmSource->AvailableFrames();
            if (availableFrames.has_value())
            {
                // The source queue already advances on its capture clock.
                // Never add wall-clock credit to it, and let its first ready
                // buffer through even on the first tick after START.
                m_elapsedFrameCredit = 0;
                const std::uint32_t maximumFrames = VirtioSoundDevice::c_maxTransferBytes / m_frameBytes;
                std::uint32_t maximumBurstFrames = maximumFrames;
                const std::uint32_t bufferFrames = m_bufferBytes / m_frameBytes;
                const std::uint32_t periodFrames = m_periodBytes / m_frameBytes;
                if (bufferFrames > periodFrames)
                {
                    // Preserve one period of guest-ring headroom. A single
                    // source burst that fills the modulo-indexed guest ring
                    // can look like no pointer movement to the consumer.
                    maximumBurstFrames = (std::min)(maximumBurstFrames, bufferFrames - periodFrames);
                }
                const std::uint32_t availableBudget = (std::min)(*availableFrames, maximumBurstFrames);
                return PumpCapture(static_cast<int>(availableBudget));
            }
        }

        if (pacingGenerationChanged)
        {
            // Activity may have begun or ended at any point inside the elapsed
            // interval. For sources without their own clock, discard it instead
            // of over-crediting paused time.
            return 0;
        }

        const std::uint32_t maxFrames = VirtioSoundDevice::c_maxTransferBytes / m_frameBytes;
        long double added = static_cast<long double>(elapsedNanoseconds) * m_format.m_sampleRate / 1000000000.0L;
        m_elapsedFrameCredit = std::min(static_cast<long double>(maxFrames), m_elapsedFrameCredit + added);
        int budget = static_cast<int>(std::min(m_elapsedFrameCredit, static_cast<long double>(maxFrames)));
        int completed = PumpCapture(budget);
        m_elapsedFrameCredit = std::max(0.0L, m_elapsedFrameCredit - completed);
        return completed;
    }

    bool CaptureRunning() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        return Ready() && IsRunning();
    }

    std::uint32_t CapturePeriodFrames() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        return m_periodBytes / m_frameBytes;
    }

    std::uint32_t MaximumCaptureFrameBudget() const noexcept
    {
        return VirtioSoundDevice::c_maxTransferBytes / m_frameBytes;
    }

    std::uint64_t CapturePacingGeneration() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        return m_pacingGeneration;
    }

    void LatchXrun()
    {
        if (!XRunEventsEnabled())
        {
            return;
        }
        m_pendingXrun = true;
        ProcessEventQueue();
    }

    int ProcessEventQueue()
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        if (!Ready() || !m_queues[1].m_enabled || !m_pendingXrun)
        {
            return 0;
        }
        try
        {
            Chain chain;
            if (Peek(m_queues[1], chain) == nullptr)
            {
                return 0;
            }
            if (chain.m_hasReadable || chain.m_capacity < 8)
            {
                throw std::runtime_error("Event buffer must be device-writable and at least 8 bytes");
            }
            std::array<std::uint8_t, 8> event{};
            WriteLe(event.data(), 0, 0x1101, 4); // PCM_XRUN, stream 0
            Scatter(chain, 0, event.data(), event.size());
            Complete(m_queues[1], chain, 8);
            m_pendingXrun = false;
            return 1;
        }
        catch (const std::exception& error)
        {
            Fault(error.what());
            return 0;
        }
    }

    void DrainCapture()
    {
        if (!m_queues[3].m_enabled)
        {
            return;
        }
        for (int n = 0; n < c_maxQueueSize; ++n)
        {
            Chain chain;
            if (PeekRx(chain) == nullptr)
            {
                return;
            }
            if (chain.m_capacity < 8)
            {
                throw std::runtime_error("Capture response has no status buffer");
            }
            CaptureError(chain, c_statusIoError, 5);
        }
    }

    void RaiseInterrupt(std::uint16_t vector, std::uint8_t isrBit)
    {
        Increment(m_interruptSignalsRequested);
        std::uint16_t control = static_cast<std::uint16_t>(ReadLe(m_pci.data(), 0x86, 2));
        if ((control & 0x8000) != 0)
        {
            if (vector < 5)
            {
                m_pendingVectors |= 1ULL << vector;
                FlushPending();
            }
            return;
        }
        m_isr |= isrBit;
        UpdateIntx();
    }

    void FlushPending()
    {
        UpdateIntx();
        std::uint16_t control = static_cast<std::uint16_t>(ReadLe(m_pci.data(), 0x86, 2));
        if ((control & 0xc000) != 0x8000)
        {
            return;
        }
        for (int i = 0; i < 5; ++i)
        {
            if ((m_pendingVectors & (1ULL << i)) != 0 && (ReadLe(m_msix.data(), i * 16 + 12, 4) & 1) == 0)
            {
                DeliverInterrupt(ReadLe(m_msix.data(), i * 16, 8), static_cast<std::uint32_t>(ReadLe(m_msix.data(), i * 16 + 8, 4)));
                m_pendingVectors &= ~(1ULL << i);
            }
        }
    }

    void DeliverInterrupt(std::uint64_t address, std::uint32_t data)
    {
        Increment(m_interruptApiRequests);
        if (!m_interrupts.m_deliverMsi(m_interrupts.m_context, address, data))
        {
            Increment(m_interruptApiFailures);
            throw std::runtime_error("MSI delivery callback failed");
        }
        Increment(m_interruptApiSuccesses);
    }

    void UpdateIntx()
    {
        if (m_interrupts.m_setIntx == nullptr)
        {
            m_intxAsserted = false;
            return;
        }
        bool asserted = m_interrupts.m_setIntx != nullptr && m_isr != 0 && (ReadLe(m_pci.data(), 0x86, 2) & 0x8000) == 0 &&
                        (ReadLe(m_pci.data(), 4, 2) & 0x400) == 0;
        if (asserted == m_intxAsserted)
        {
            return;
        }
        m_intxAsserted = asserted;
        if (asserted)
        {
            m_pci[6] |= 8;
        }
        else
        {
            m_pci[6] &= 0xf7;
        }
        if (!m_interrupts.m_setIntx(m_interrupts.m_context, asserted))
        {
            throw std::runtime_error("INTx callback failed");
        }
    }

    DiagnosticSnapshot GetDiagnosticsSnapshot() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_gate);
        const Queue& rx = m_queues[3];
        DiagnosticSnapshot snapshot{};
        snapshot.m_rxQueueEnabled = rx.m_enabled;
        snapshot.m_rxLastAvailableIndex = rx.m_lastAvailable;
        snapshot.m_rxObservedAvailableIndex = rx.m_observedAvailable;
        snapshot.m_rxOutstandingAvailable = static_cast<std::uint16_t>(rx.m_observedAvailable - rx.m_lastAvailable);
        snapshot.m_rxNextUsedIndex = rx.m_used;
        snapshot.m_resetCount = m_diagnosticResetCount;
        snapshot.m_rxAvailableProcessed = m_rxAvailableProcessed;
        snapshot.m_rxUsedEntriesPublished = m_rxUsedEntriesPublished;
        snapshot.m_rxDataCompletions = m_rxDataCompletions;
        snapshot.m_rxStatusOnlyCompletions = m_rxStatusOnlyCompletions;
        snapshot.m_notificationRequests = m_notificationRequests;
        snapshot.m_notificationFailures = m_notificationFailures;
        snapshot.m_interruptSignalsRequested = m_interruptSignalsRequested;
        snapshot.m_interruptApiRequests = m_interruptApiRequests;
        snapshot.m_interruptApiSuccesses = m_interruptApiSuccesses;
        snapshot.m_interruptApiFailures = m_interruptApiFailures;
        return snapshot;
    }
};

VirtioSoundDevice::VirtioSoundDevice(GuestMemoryCallbacks guestMemory, InterruptCallbacks interrupts, std::shared_ptr<PcmSource> pcmSource, PcmFormat format) :
    m_impl(std::make_unique<Impl>(guestMemory, interrupts, std::move(pcmSource), format))
{
}

VirtioSoundDevice::~VirtioSoundDevice() = default;

std::uint32_t VirtioSoundDevice::ReadConfig(std::uint32_t offset, std::uint8_t width)
{
    return m_impl->ReadConfig(offset, width);
}

void VirtioSoundDevice::WriteConfig(std::uint32_t offset, std::uint32_t value, std::uint8_t width)
{
    m_impl->WriteConfig(offset, value, width);
}

void VirtioSoundDevice::ReadBar(std::uint64_t offset, std::uint8_t* destination, std::size_t byteCount)
{
    m_impl->ReadBar(offset, destination, byteCount);
}

void VirtioSoundDevice::WriteBar(std::uint64_t offset, const std::uint8_t* source, std::size_t byteCount)
{
    m_impl->WriteBar(offset, source, byteCount);
}

int VirtioSoundDevice::ProcessControlQueue()
{
    return m_impl->ProcessControlQueue();
}
int VirtioSoundDevice::ProcessEventQueue()
{
    return m_impl->ProcessEventQueue();
}
int VirtioSoundDevice::PumpCapture(int frameBudget)
{
    return m_impl->PumpCapture(frameBudget);
}
int VirtioSoundDevice::PumpCaptureElapsed(std::uint64_t elapsedNanoseconds)
{
    return m_impl->PumpCaptureElapsed(elapsedNanoseconds);
}
bool VirtioSoundDevice::CaptureRunning() const noexcept
{
    return m_impl->CaptureRunning();
}
std::uint32_t VirtioSoundDevice::CapturePeriodFrames() const noexcept
{
    return m_impl->CapturePeriodFrames();
}
std::uint32_t VirtioSoundDevice::MaximumCaptureFrameBudget() const noexcept
{
    return m_impl->MaximumCaptureFrameBudget();
}
std::uint64_t VirtioSoundDevice::CapturePacingGeneration() const noexcept
{
    return m_impl->CapturePacingGeneration();
}
void VirtioSoundDevice::Reset()
{
    m_impl->Reset();
}

std::uint8_t VirtioSoundDevice::DeviceStatus() const
{
    std::lock_guard<std::recursive_mutex> lock(m_impl->m_gate);
    return m_impl->m_status;
}

std::string VirtioSoundDevice::LastFault() const
{
    std::lock_guard<std::recursive_mutex> lock(m_impl->m_gate);
    return m_impl->m_lastFault;
}

DiagnosticSnapshot VirtioSoundDevice::GetDiagnosticsSnapshot() const
{
    return m_impl->GetDiagnosticsSnapshot();
}

} // namespace wsl::virtio_sound::native
