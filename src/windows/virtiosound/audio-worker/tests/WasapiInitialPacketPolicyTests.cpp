// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WasapiInitialPacketPolicy.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using wsl::virtio_sound::audio_worker::WasapiInitialPacketPolicy;
using wsl::virtio_sound::audio_worker::WasapiPacketFlagDecision;

constexpr std::uint32_t c_dataDiscontinuity = 0x00000001;
constexpr std::uint32_t c_silent = 0x00000002;
constexpr std::uint32_t c_timestampError = 0x00000004;
constexpr std::uint32_t c_packetFrames = 480;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

WasapiPacketFlagDecision Evaluate(
    WasapiInitialPacketPolicy& policy, std::uint32_t rawFlags, std::uint32_t frames = c_packetFrames, bool packetValid = true, bool bufferReleased = true)
{
    return policy.Evaluate(rawFlags, c_dataDiscontinuity, c_timestampError, frames, packetValid, bufferReleased);
}

void TestInitialEmptyReadDoesNotSetBaseline()
{
    WasapiInitialPacketPolicy policy;
    const auto empty = Evaluate(policy, 0, 0);
    Require(!policy.HasNonemptyPacketBaseline(), "an empty read must not establish the nonempty packet baseline");
    Require(
        !empty.m_traceStartupDataDiscontinuity && !empty.m_discontinuity,
        "an empty clean read must not report a startup boundary or discontinuity");

    const auto firstNonempty = Evaluate(policy, c_dataDiscontinuity);
    Require(
        firstNonempty.m_traceStartupDataDiscontinuity && !firstNonempty.m_discontinuity && policy.HasNonemptyPacketBaseline(),
        "the first nonempty flagged packet after an empty read must establish the baseline");
}

void TestInitialDataDiscontinuityIsAcceptedAndTraceable()
{
    WasapiInitialPacketPolicy policy;
    const std::uint32_t rawFlags = c_dataDiscontinuity | c_silent;
    const auto decision = Evaluate(policy, rawFlags);
    Require(decision.m_traceStartupDataDiscontinuity, "the adapter must receive a startup-boundary signal to trace");
    Require(!decision.m_discontinuity, "the initial data discontinuity must not reach the worker as fatal");
    Require(
        decision.m_rawFlags == rawFlags && (decision.m_rawFlags & c_silent) != 0,
        "the policy must preserve original WASAPI flags unchanged");
    Require(policy.HasNonemptyPacketBaseline(), "the accepted initial packet must establish the baseline");
}

void TestSubsequentDataDiscontinuityRemainsFatal()
{
    WasapiInitialPacketPolicy policy;
    const auto first = Evaluate(policy, c_dataDiscontinuity);
    Require(
        first.m_traceStartupDataDiscontinuity && !first.m_discontinuity,
        "the first data discontinuity should be treated as the startup boundary");

    const auto later = Evaluate(policy, c_dataDiscontinuity);
    Require(
        !later.m_traceStartupDataDiscontinuity && later.m_discontinuity,
        "a data discontinuity after the baseline must remain fatal");
}

void TestCleanFirstPacketMakesSecondDataDiscontinuityFatal()
{
    WasapiInitialPacketPolicy policy;
    const auto first = Evaluate(policy, 0);
    Require(!first.m_discontinuity && policy.HasNonemptyPacketBaseline(), "a clean first packet must establish the baseline");

    const auto second = Evaluate(policy, c_dataDiscontinuity);
    Require(
        second.m_discontinuity && !second.m_traceStartupDataDiscontinuity,
        "a data discontinuity on packet two must remain fatal after a clean start");
}

void TestInitialTimestampErrorRemainsFatal()
{
    WasapiInitialPacketPolicy policy;
    const auto first = Evaluate(policy, c_timestampError);
    Require(first.m_discontinuity, "timestamp errors on the first nonempty packet must remain fatal");
    Require(
        policy.HasNonemptyPacketBaseline(),
        "a valid, released packet with a timestamp error still establishes the stream baseline");
}

void TestNewPolicyInstanceResetsTheStreamBaseline()
{
    WasapiInitialPacketPolicy firstStream;
    const auto firstStreamPacket = Evaluate(firstStream, 0);
    Require(
        !firstStreamPacket.m_discontinuity && firstStream.HasNonemptyPacketBaseline(),
        "the first stream should establish its own baseline");

    WasapiInitialPacketPolicy secondStream;
    const auto secondStreamPacket = Evaluate(secondStream, c_dataDiscontinuity);
    Require(
        secondStreamPacket.m_traceStartupDataDiscontinuity && !secondStreamPacket.m_discontinuity,
        "a new policy instance must accept its own first data discontinuity");
}

void TestOnlyValidReleasedNonemptyPacketsEstablishBaseline()
{
    WasapiInitialPacketPolicy policy;
    (void)Evaluate(policy, c_dataDiscontinuity, c_packetFrames, false, true);
    Require(!policy.HasNonemptyPacketBaseline(), "an invalid packet must not establish the baseline");

    (void)Evaluate(policy, c_dataDiscontinuity, c_packetFrames, true, false);
    Require(
        !policy.HasNonemptyPacketBaseline(),
        "a packet whose buffer was not released successfully must not establish the baseline");

    const auto valid = Evaluate(policy, c_dataDiscontinuity, c_packetFrames, true, true);
    Require(
        valid.m_traceStartupDataDiscontinuity && policy.HasNonemptyPacketBaseline(),
        "the first valid, released, nonempty packet must establish the baseline");
}

} // namespace

int main()
{
    try
    {
        TestInitialEmptyReadDoesNotSetBaseline();
        TestInitialDataDiscontinuityIsAcceptedAndTraceable();
        TestSubsequentDataDiscontinuityRemainsFatal();
        TestCleanFirstPacketMakesSecondDataDiscontinuityFatal();
        TestInitialTimestampErrorRemainsFatal();
        TestNewPolicyInstanceResetsTheStreamBaseline();
        TestOnlyValidReleasedNonemptyPacketsEstablishBaseline();
        std::cout << "WASAPI initial packet policy tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WASAPI initial packet policy test failure: " << error.what() << '\n';
        return 1;
    }
}
