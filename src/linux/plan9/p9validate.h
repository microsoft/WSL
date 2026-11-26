// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9protohelpers.h"
#include "p9data.h"

namespace p9fs {

// Keep variable-length layouts in sync with GetMessageSize and TraceLogMessage.
inline bool ValidateMessageStructure(MessageType messageType, gsl::span<const gsl::byte> message)
{
    const auto minimumSize = GetMessageSize(messageType);
    if (minimumSize == 0 || message.size() < minimumSize)
    {
        return false;
    }

    SpanReader reader{message};
    reader.Read(HeaderSize);

    switch (messageType)
    {
    case MessageType::Tversion:
    case MessageType::Rversion:
        return reader.TryRead(4).Success && reader.TryString().Success;

    case MessageType::Twalk:
    case MessageType::Twopen:
    {
        const auto fixedSize = messageType == MessageType::Twalk ? 8 : 32;
        if (!reader.TryRead(fixedSize).Success)
        {
            return false;
        }

        const auto count = reader.TryU16();
        if (!count.Success || count.Result > (reader.Size() - reader.Offset()) / sizeof(UINT16))
        {
            return false;
        }

        for (UINT16 i = 0; i < count.Result; ++i)
        {
            if (!reader.TryString().Success)
            {
                return false;
            }
        }

        return true;
    }

    case MessageType::Rwalk:
    {
        const auto count = reader.TryU16();
        return count.Success && reader.TryRead(count.Result * QidSize).Success;
    }

    case MessageType::Twrite:
    case MessageType::Rread:
    case MessageType::Rreaddir:
    case MessageType::Rwreaddir:
    {
        if (messageType == MessageType::Twrite && !reader.TryRead(12).Success)
        {
            return false;
        }

        const auto count = reader.TryU32();
        return count.Success && reader.TryRead(count.Result).Success;
    }

    case MessageType::Tauth:
    case MessageType::Tattach:
    {
        const auto fixedSize = messageType == MessageType::Tauth ? 4 : 8;
        return reader.TryRead(fixedSize).Success && reader.TryString().Success && reader.TryString().Success && reader.TryRead(4).Success;
    }

    case MessageType::Tlcreate:
    case MessageType::Txattrcreate:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryRead(12).Success;

    case MessageType::Tsymlink:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryString().Success && reader.TryRead(4).Success;

    case MessageType::Tmknod:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryRead(16).Success;

    case MessageType::Trename:
    case MessageType::Txattrwalk:
    case MessageType::Tlink:
        return reader.TryRead(8).Success && reader.TryString().Success;

    case MessageType::Rreadlink:
        return reader.TryString().Success;

    case MessageType::Tlock:
        return reader.TryRead(4 + 1 + 4 + 8 + 8 + 4).Success && reader.TryString().Success;

    case MessageType::Tgetlock:
        return reader.TryRead(4 + 1 + 8 + 8 + 4).Success && reader.TryString().Success;

    case MessageType::Rgetlock:
        return reader.TryRead(1 + 8 + 8 + 4).Success && reader.TryString().Success;

    case MessageType::Tmkdir:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryRead(8).Success;

    case MessageType::Trenameat:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryRead(4).Success && reader.TryString().Success;

    case MessageType::Tunlinkat:
        return reader.TryRead(4).Success && reader.TryString().Success && reader.TryRead(4).Success;

    case MessageType::Rwopen:
        return reader.TryRead(1 + 2 + QidSize).Success && reader.TryString().Success && reader.TryRead(4 * 4 + 15 * 8).Success;

    default:
        // Fixed-size messages were checked above.
        return true;
    }
}

} // namespace p9fs
