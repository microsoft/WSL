// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "VirtioFsShareRequest.h"

using namespace wsl::windows::service;

std::vector<char> wsl::windows::service::ProcessVirtioFsShareRequest(
    _In_ gsl::span<gsl::byte> Request,
    _In_ const AddVirtioFsShareCallback& AddShare,
    _In_ const RemountVirtioFsShareCallback& RemountShare)
{
    const auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(Request);
    THROW_HR_IF(E_UNEXPECTED, !header);

    WSL_LOG("VirtiofsMessageRequest", TraceLoggingValue(header->PrettyPrint().c_str(), "Content"));

    auto buildResponse = [header](const VirtioFsShareResult& Share, HRESULT Result) {
        wsl::shared::MessageWriter<LX_INIT_ADD_VIRTIOFS_SHARE_RESPONSE_MESSAGE> response(
            LxInitMessageAddVirtioFsDeviceResponse);
        response->Result = SUCCEEDED(Result) ? 0 : EINVAL;
        response.WriteString(response->TagOffset, Share.Tag);
        response.WriteString(response->ChildNameOffset, Share.ChildName);
        response.WriteString(response->SourceOffset, Share.Source);
        response->Header.TransactionId = header->TransactionId;
        response->Header.TransactionStep = static_cast<unsigned int>(TRANSACTION_STEP::FIRST_REPLY);

        WSL_LOG("VirtiofsMessageResponse", TraceLoggingValue(response->PrettyPrint().c_str(), "Content"));

        const auto span = response.Span();
        return std::vector<char>(
            reinterpret_cast<const char*>(span.data()),
            reinterpret_cast<const char*>(span.data()) + span.size());
    };

    VirtioFsShareResult share;
    HRESULT result;
    if (header->MessageType == LxInitMessageAddVirtioFsDevice)
    {
        result = wil::ResultFromException([&] {
            const auto* request = gslhelpers::try_get_struct<LX_INIT_ADD_VIRTIOFS_SHARE_MESSAGE>(Request);
            THROW_HR_IF(E_UNEXPECTED, !request);

            const auto path = wsl::shared::string::MultiByteToWide(
                wsl::shared::string::FromSpan(Request, request->PathOffset));
            const auto options = wsl::shared::string::MultiByteToWide(
                wsl::shared::string::FromSpan(Request, request->OptionsOffset));
            share = AddShare(request->Admin, path, options);
        });
    }
    else if (header->MessageType == LxInitMessageRemountVirtioFsDevice)
    {
        result = wil::ResultFromException([&] {
            const auto* request = gslhelpers::try_get_struct<LX_INIT_REMOUNT_VIRTIOFS_SHARE_MESSAGE>(Request);
            THROW_HR_IF(E_UNEXPECTED, !request);

            const auto tag = wsl::shared::string::MultiByteToWide(
                wsl::shared::string::FromSpan(Request, request->TagOffset));
            share = RemountShare(tag, request->Admin);
        });
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected MessageType %d", header->MessageType);
    }

    return buildResponse(share, result);
}