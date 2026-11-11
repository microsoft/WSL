// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>

namespace wsl::virtio_sound::audio_worker {

struct WasapiPacketFlagDecision
{
    std::uint32_t m_rawFlags{};
    bool m_traceStartupDataDiscontinuity{};
    bool m_discontinuity{};
};

// Keeps the first valid, nonempty, successfully released packet as the
// baseline for one WASAPI stream. The flag masks are supplied by the adapter
// so this policy can be tested without Windows headers or a capture device.
class WasapiInitialPacketPolicy final
{
public:
    WasapiPacketFlagDecision Evaluate(
        std::uint32_t rawFlags, std::uint32_t dataDiscontinuityMask, std::uint32_t timestampErrorMask, std::uint32_t frames, bool packetValid, bool bufferReleased) noexcept
    {
        const bool nonemptyValidReleasedPacket = packetValid && bufferReleased && frames != 0;
        const bool dataDiscontinuity = (rawFlags & dataDiscontinuityMask) != 0;
        const bool timestampError = (rawFlags & timestampErrorMask) != 0;
        const bool startupDataDiscontinuity = nonemptyValidReleasedPacket && !m_hasNonemptyPacketBaseline && dataDiscontinuity;

        if (nonemptyValidReleasedPacket)
        {
            m_hasNonemptyPacketBaseline = true;
        }

        return {rawFlags, startupDataDiscontinuity, timestampError || (dataDiscontinuity && !startupDataDiscontinuity)};
    }

    bool HasNonemptyPacketBaseline() const noexcept
    {
        return m_hasNonemptyPacketBaseline;
    }

private:
    bool m_hasNonemptyPacketBaseline{};
};

} // namespace wsl::virtio_sound::audio_worker
