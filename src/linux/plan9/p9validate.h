// Copyright (C) Microsoft Corporation. All rights reserved.
// Keep message layouts in sync with GetMessageSize in p9data.h and TraceLogMessage in p9log.h.
#pragma once

#include "p9protohelpers.h"

namespace p9fs {

// Validates that a complete message can be read without crossing its declared boundary.
inline bool ValidateMessageStructure(MessageType messageType, gsl::span<const gsl::byte> message)
{
    SpanReader reader{message};
    if (!reader.TryRead(HeaderSize).Success)
    {
        return false;
    }

    switch (messageType)
    {
    case MessageType::Tversion:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rversion:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tflush:
    {
        if (!reader.TryRead(2).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rflush:
    {
        return true;
    }

    case MessageType::Twalk:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        auto nwname = reader.TryU16();
        if (!nwname.Success)
        {
            return false;
        }
        if (nwname.Result > (reader.Size() - reader.Offset()) / sizeof(UINT16))
        {
            return false;
        }

        for (decltype(nwname.Result) i{}; i < nwname.Result; ++i)
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
        auto nwqid = reader.TryU16();
        if (!nwqid.Success)
        {
            return false;
        }
        if (nwqid.Result > (reader.Size() - reader.Offset()) / 13)
        {
            return false;
        }

        for (decltype(nwqid.Result) i{}; i < nwqid.Result; ++i)
        {
            if (!reader.TryRead(13).Success)
            {
                return false;
            }
        }
        return true;
    }

    case MessageType::Tread:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rread:
    {
        auto count = reader.TryU32();
        if (!count.Success)
        {
            return false;
        }
        if (!reader.TryRead(count.Result).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Twrite:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        auto count = reader.TryU32();
        if (!count.Success)
        {
            return false;
        }
        if (!reader.TryRead(count.Result).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rwrite:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tclunk:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rclunk:
    {
        return true;
    }

    case MessageType::Tremove:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rremove:
    {
        return true;
    }

    case MessageType::Tauth:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rauth:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tattach:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rattach:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rlerror:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tstatfs:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rstatfs:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tlopen:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rlopen:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tlcreate:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rlcreate:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tsymlink:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rsymlink:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tmknod:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rmknod:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Trename:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rrename:
    {
        return true;
    }

    case MessageType::Treadlink:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rreadlink:
    {
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tgetattr:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rgetattr:
    {
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tsetattr:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rsetattr:
    {
        return true;
    }

    case MessageType::Txattrwalk:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rxattrwalk:
    {
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Txattrcreate:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rxattrcreate:
    {
        return true;
    }

    case MessageType::Treaddir:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rreaddir:
    {
        auto count = reader.TryU32();
        if (!count.Success)
        {
            return false;
        }
        if (!reader.TryRead(count.Result).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tfsync:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rfsync:
    {
        return true;
    }

    case MessageType::Tlock:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(1).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rlock:
    {
        if (!reader.TryRead(1).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tgetlock:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(1).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rgetlock:
    {
        if (!reader.TryRead(1).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Tlink:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rlink:
    {
        return true;
    }

    case MessageType::Tmkdir:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rmkdir:
    {
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Trenameat:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rrenameat:
    {
        return true;
    }

    case MessageType::Tunlinkat:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Runlinkat:
    {
        return true;
    }

    case MessageType::Taccess:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Raccess:
    {
        return true;
    }

    case MessageType::Twreaddir:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Rwreaddir:
    {
        auto count = reader.TryU32();
        if (!count.Success)
        {
            return false;
        }
        if (!reader.TryRead(count.Result).Success)
        {
            return false;
        }
        return true;
    }

    case MessageType::Twopen:
    {
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        auto nwname = reader.TryU16();
        if (!nwname.Success)
        {
            return false;
        }
        if (nwname.Result > (reader.Size() - reader.Offset()) / sizeof(UINT16))
        {
            return false;
        }

        for (decltype(nwname.Result) i{}; i < nwname.Result; ++i)
        {
            if (!reader.TryString().Success)
            {
                return false;
            }
        }
        return true;
    }

    case MessageType::Rwopen:
    {
        if (!reader.TryRead(1).Success)
        {
            return false;
        }
        if (!reader.TryRead(2).Success)
        {
            return false;
        }
        if (!reader.TryRead(13).Success)
        {
            return false;
        }
        if (!reader.TryString().Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(4).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        if (!reader.TryRead(8).Success)
        {
            return false;
        }
        return true;
    }

    default:
        return false;
    }
}

} // namespace p9fs
