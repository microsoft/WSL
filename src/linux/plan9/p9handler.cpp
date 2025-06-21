// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9protohelpers.h"
#include "p9errors.h"
#include "p9defs.h"
#include "p9tracelogging.h"
#include "p9tracelogginghelper.h"
#include "p9log.h"
#include "p9data.h"
#include "p9await.h"
#include "p9fid.h"
#include "p9handler.h"
#include "p9commonutil.h"

namespace p9fs {

// Size to use for the stack-allocated response buffer.
constexpr UINT32 c_staticBufferSize = 256;

constexpr UINT32 c_createRetryCount = 3;

// Handler for 9pfs protocol messages.
class Handler final : public IHandler
{
public:
    Handler(ISocket& s, IShareList& shareList) noexcept :
        m_Socket{&s}, m_Requests{std::make_shared<RequestList>()}, m_ShareList{shareList}
    {
    }

    Handler(IShareList& shareList, bool allowRenegotiate) noexcept :
        m_Requests{std::make_shared<RequestList>()}, m_AllowRenegotiate{allowRenegotiate}, m_ShareList{shareList}
    {
    }

private:
    // Encapsulates the buffer and SpanWriter used for sending a response to the client.
    class MessageResponse final
    {
    public:
        // Initializes a new MessageResponse with the specified buffer.
        MessageResponse(gsl::span<gsl::byte> initialBuffer, bool allowResize = true) :
            Writer{initialBuffer}, m_allowResize{allowResize}
        {
            // Skip the header, which will be written last.
            Writer.Next(HeaderSize);
        }

        // Checks if the current buffer is large enough for the message, taking any additional
        // dynamic values into account. If not, a new buffer is allocated and used to write the
        // response.
        void EnsureSize(MessageType message, UINT32 extraSize, UINT32 maxSize)
        {
            // Ensure this function is called for a response message (which are always odd).
            WI_ASSERT(static_cast<int>(message) % 2 == 1);

            auto size = static_cast<UINT64>(GetMessageSize(message)) + extraSize;

            // Check if the message is larger than the negotiated size. This could happen if the
            // client is sending invalid requests.
            if (size > maxSize)
            {
                THROW_INVALID();
            }

            // If the message is larger than the stack buffer, allocate a dynamic buffer and
            // update the writer.
            // N.B. This is not allowed if the initial buffer was based on a virtio write span.
            if (size > Writer.MaxSize())
            {
                if (!m_allowResize)
                {
                    Plan9TraceLoggingProvider::InvalidResponseBufferSize();
                    THROW_INVALID();
                }

                m_dynamicBuffer.resize(size);
                Writer = SpanWriter{m_dynamicBuffer};

                // Skip the header, which will be written last.
                Writer.Next(HeaderSize);
            }
        }

        SpanWriter Writer;

    private:
        MessageResponse(const MessageResponse&) = delete;
        MessageResponse& operator=(const MessageResponse&) = delete;

        std::vector<gsl::byte> m_dynamicBuffer;
        bool m_allowResize;
    };

    // Encapsulates information about a request in progress, which is used by Tflush to wait
    // for completion before responding.
    struct RequestInfo
    {
        RequestInfo(UINT16 tag) : Tag{tag}
        {
        }

        // Since this is part of a linked list, make sure it's never moved or copied.
        RequestInfo(const RequestInfo&) = delete;
        RequestInfo& operator=(const RequestInfo&) = delete;
        RequestInfo(RequestInfo&&) = delete;
        RequestInfo& operator=(RequestInfo&&) = delete;

        LIST_ENTRY Link;
        AsyncEvent Event;
        UINT16 Tag;
        bool Cancelled{};
    };

    struct RequestList
    {
        std::mutex Lock;
        util::LinkedList<RequestInfo> Requests;
    };

    class RequestTracker
    {
    public:
        RequestTracker(const std::shared_ptr<RequestList>& requests, UINT16 tag) :
            m_requestList{requests}, m_request{std::make_unique<RequestInfo>(tag)}
        {
            // Insert into the list on construction.
            std::lock_guard<std::mutex> lock{m_requestList->Lock};
            m_requestList->Requests.Insert(*m_request);
        }

        RequestTracker(RequestTracker&& other) = default;

        ~RequestTracker()
        {
            // The pointers can be invalid if this instance has been moved.
            if (!m_requestList || !m_request)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock{m_requestList->Lock};

                // Remove the request from the list of pending requests. This means that Cancelled
                // can't change after the lock is dropped, since HandleFlush can't find the request
                // anymore.
                m_requestList->Requests.Remove(*m_request);
            }

            // If the request is marked Cancelled, it means a Tflush has taken ownership of this
            // pointer and is waiting for the event. The pointer may become invalid as soon as the
            // event is set.
            // N.B. A shared_ptr would be nicer, but Tflush can only get the RequestInfo itself
            //      from the linked list, so it wouldn't be able to access the shared_ptr's
            //      reference count. Therefore, this ownership shuffling is required.
            auto& localRequest = *m_request;
            if (m_request->Cancelled)
            {
                m_request.release();
            }

            localRequest.Event.Set();
        }

        RequestInfo& Request() const
        {
            return *m_request;
        }

    private:
        std::shared_ptr<RequestList> m_requestList;
        std::unique_ptr<RequestInfo> m_request;
    };

    void LogMessage([[maybe_unused]] gsl::span<const gsl::byte> message)
    {
        TraceLogMessage(message);
    }

    Task<LX_INT> HandleMessage(MessageType messageType, SpanReader& reader, MessageResponse& response)
    {
        // Handle async operations.
        switch (messageType)
        {
        case MessageType::Tread:
            co_return co_await HandleRead(reader, response);

        case MessageType::Twrite:
            co_return co_await HandleWrite(reader, response);

        case MessageType::Tflush:
            co_return co_await HandleFlush(reader);

        default:
            // Default label prevents warning in clang.
            break;
        }

        // Handle blocking operations.
        co_return co_await BlockingCode([&]() -> LX_INT {
            switch (messageType)
            {
            case MessageType::Tstatfs:
                return HandleStatFs(reader, response);

            case MessageType::Tlopen:
                return HandleLOpen(reader, response);

            case MessageType::Tlcreate:
                return HandleLCreate(reader, response);

            case MessageType::Tsymlink:
                return HandleSymLink(reader, response);

            case MessageType::Tmknod:
                return HandleMkNod(reader, response);

            case MessageType::Treadlink:
                return HandleReadLink(reader, response);

            case MessageType::Tgetattr:
                return HandleGetAttr(reader, response);

            case MessageType::Tsetattr:
                return HandleSetAttr(reader);

            case MessageType::Txattrwalk:
                return HandleXattrWalk(reader, response);

            case MessageType::Txattrcreate:
                return HandleXattrCreate(reader);

            case MessageType::Treaddir:
            case MessageType::Twreaddir:
                return HandleReadDir(reader, response, messageType == MessageType::Twreaddir);

            case MessageType::Tfsync:
                return HandleFsync(reader);

            case MessageType::Tlock:
                return HandleLock(reader, response);

            case MessageType::Tgetlock:
                return HandleGetLock(reader, response);

            case MessageType::Tlink:
                return HandleLink(reader);

            case MessageType::Tmkdir:
                return HandleMkDir(reader, response);

            case MessageType::Trenameat:
                return HandleRenameAt(reader);

            case MessageType::Tunlinkat:
                return HandleUnlinkAt(reader);

            case MessageType::Tversion:
                return HandleVersion(reader, response);

            case MessageType::Tauth:
                return HandleNotSupported("auth");

            case MessageType::Tattach:
                return HandleAttach(reader, response);

            case MessageType::Twalk:
                return HandleWalk(reader, response);

            case MessageType::Tclunk:
                return HandleClunk(reader);

            case MessageType::Tremove:
                return HandleRemove(reader);

            case MessageType::Trename:
                return HandleRename(reader);

            case MessageType::Taccess:
                return HandleAccess(reader);

            case MessageType::Twopen:
                return HandleWOpen(reader, response);

            default:
                return LX_ENOTSUP;
            }
        });
    }

    LX_INT HandleNotSupported(PCSTR)
    {
        return LX_ENOTSUP;
    }

    LX_INT HandleVersion(SpanReader& reader, MessageResponse& response)
    {
        // Tversion can only be sent once, unless it's specifically allowed multiple times which
        // is used for virtio.
        if (m_Negotiated && !m_AllowRenegotiate)
        {
            return LX_ENOTSUP;
        }

        auto size = reader.U32();
        auto version = reader.String();

        if (size < MinimumRequestBufferSize)
        {
            return LX_ENOTSUP;
        }

        bool use9P2000W = false;
        if (version == ProtocolVersionW)
        {
            use9P2000W = true;
        }
        else if (version != ProtocolVersionL)
        {
            return LX_ENOTSUP;
        }

        size = std::min(size, MaximumRequestBufferSize);

        // If Tversion was allowed more than once, still require the values to match the previously
        // negotiated values.
        if (m_Negotiated && (use9P2000W != m_Use9P2000W || size != m_NegotiatedSize))
        {
            return LX_ENOTSUP;
        }

        m_Use9P2000W = use9P2000W;
        m_NegotiatedSize = size;
        m_Negotiated = true;
        response.EnsureSize(MessageType::Rversion, static_cast<UINT32>(version.size()), m_NegotiatedSize);
        response.Writer.U32(size);
        response.Writer.String(version);
        return {};
    }

    LX_INT HandleAttach(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        reader.U32();    // afid (unused)
        reader.String(); // uname (unused)
        auto aname = reader.String();
        auto uid = reader.U32();

        auto root = m_ShareList.MakeRoot(std::string_view{aname.data(), gsl::narrow_cast<size_t>(aname.size())}, uid);
        if (!root)
        {
            return root.Error();
        }

        auto result = CreateFile(root.Get(), uid);
        if (!result)
        {
            return result.Error();
        }

        auto [file, qid] = result.Get();

        EmplaceFid(fid, file);

        response.EnsureSize(MessageType::Rattach, 0, m_NegotiatedSize);
        response.Writer.Qid(qid);
        return {};
    }

    LX_INT HandleStatFs(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();

        const auto file = LookupFid(fid);

        auto result = file->StatFs();
        if (!result)
        {
            return result.Error();
        }

        auto& statfs = result.Get();
        response.EnsureSize(MessageType::Rstatfs, 0, m_NegotiatedSize);
        response.Writer.U32(statfs.Type);
        response.Writer.U32(statfs.BlockSize);
        response.Writer.U64(statfs.Blocks);
        response.Writer.U64(statfs.BlocksFree);
        response.Writer.U64(statfs.BlocksAvailable);
        response.Writer.U64(statfs.Files);
        response.Writer.U64(statfs.FilesFree);
        response.Writer.U64(statfs.FsId);
        response.Writer.U32(statfs.NameLength);
        return {};
    }

    LX_INT HandleGetAttr(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        const auto mask = reader.U64();
        const auto file = LookupFid(fid);

        auto result = file->GetAttr(mask);
        if (!result)
        {
            return result.Error();
        }

        auto& [valid, qid, stat] = result.Get();
        response.EnsureSize(MessageType::Rgetattr, 0, m_NegotiatedSize);
        response.Writer.U64(valid);
        response.Writer.Qid(qid);
        util::SpanWriteStatResult(response.Writer, stat);
        response.Writer.U64(0); // btime sec (reserved)
        response.Writer.U64(0); // btime nsec (reserved)
        response.Writer.U64(0); // gen (reserved)
        response.Writer.U64(0); // data version (reserved)

        return {};
    }

    LX_INT HandleWalk(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        const auto newfid = reader.U32();
        const auto nameCount = reader.U16();
        std::vector<std::string_view> names;
        for (auto i = 0u; i < nameCount; ++i)
        {
            auto name = reader.Name();
            names.push_back(name);
        }

        const auto entry = LookupFid(fid);
        const auto newFile = entry->Clone();

        response.EnsureSize(MessageType::Rwalk, nameCount * QidSize, m_NegotiatedSize);
        response.Writer.U16(nameCount);
        for (const auto& name : names)
        {
            auto qid = newFile->Walk(name);
            if (!qid)
            {
                return qid.Error();
            }

            response.Writer.Qid(qid.Get());
        }

        EmplaceFid(newfid, newFile);
        return {};
    }

    LX_INT HandleClunk(SpanReader& reader)
    {
        const auto fid = reader.U32();

        std::shared_ptr<Fid> item;

        {
            std::lock_guard<std::shared_mutex> lock{m_FidsLock};
            const auto iterator = m_Fids.find(fid);
            if (iterator == m_Fids.end())
            {
                return LX_EINVAL;
            }

            item = std::move(iterator->second);
            // Erase regardless of whether the clunk call succeeded.
            m_Fids.erase(iterator);
        }

        return item->Clunk();
    }

    LX_INT HandleLOpen(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto flags = reader.U32();

        const auto entry = LookupFid(fid);
        auto qid = entry->Open(static_cast<OpenFlags>(flags));
        if (!qid)
        {
            return qid.Error();
        }

        response.EnsureSize(MessageType::Rlopen, 0, m_NegotiatedSize);
        response.Writer.Qid(qid.Get());
        response.Writer.U32(IoUnit());
        return {};
    }

    LX_INT HandleLCreate(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto name = reader.Name();
        auto flags = reader.U32();
        const auto mode = reader.U32();
        const auto gid = reader.U32();

        const auto file = LookupFid(fid);
        auto qid = file->Create(name, static_cast<OpenFlags>(flags), mode, gid);
        if (!qid)
        {
            return qid.Error();
        }

        response.EnsureSize(MessageType::Rlcreate, 0, m_NegotiatedSize);
        response.Writer.Qid(qid.Get());
        response.Writer.U32(IoUnit());
        return {};
    }

    LX_INT HandleSymLink(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto name = reader.Name();
        auto target = reader.String();
        const auto gid = reader.U32();

        const auto file = LookupFid(fid);
        auto result = file->SymLink(name, target, gid);
        if (!result)
        {
            return result.Error();
        }

        response.EnsureSize(MessageType::Rsymlink, 0, m_NegotiatedSize);
        response.Writer.Qid(result.Get());
        return {};
    }

    LX_INT HandleMkNod(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto name = reader.Name();
        const auto mode = reader.U32();
        const auto major = reader.U32();
        const auto minor = reader.U32();
        const auto gid = reader.U32();

        const auto file = LookupFid(fid);
        auto result = file->MkNod(name, mode, major, minor, gid);
        if (!result)
        {
            return result.Error();
        }

        response.EnsureSize(MessageType::Rmknod, 0, m_NegotiatedSize);
        response.Writer.Qid(result.Get());
        return {};
    }

    LX_INT HandleReadLink(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();

        const auto file = LookupFid(fid);

        // The actual size of the symlink is unknown at this point, so allocate a buffer that is
        // large enough for the biggest possible target.
        response.EnsureSize(MessageType::Rreadlink, LX_PATH_MAX, m_NegotiatedSize);
        auto buffer = response.Writer.Peek().subspan(sizeof(UINT16));
        auto charSpan = gsl::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size());
        auto result = file->ReadLink(charSpan);
        if (!result)
        {
            return result.Error();
        }

        // Write the string length; we cannot use .String() because the string
        // data has already been written.
        response.Writer.U16(static_cast<UINT16>(result.Get()));
        response.Writer.Next(result.Get());
        return {};
    }

    LX_INT HandleReadDir(SpanReader& reader, MessageResponse& response, bool includeAttributes)
    {
        if (includeAttributes && !m_Use9P2000W)
        {
            return LX_ENOTSUP;
        }

        const auto fid = reader.U32();
        const auto offset = reader.U64();
        auto count = reader.U32();

        const auto file = LookupFid(fid);

        response.EnsureSize(MessageType::Rreaddir, count, m_NegotiatedSize);
        SpanWriter direntWriter{response.Writer.Peek().subspan(sizeof(UINT32), count)};
        auto error = file->ReadDir(offset, direntWriter, includeAttributes);
        if (error != 0)
        {
            return error;
        }

        auto written = direntWriter.Result().size();
        response.Writer.U32(static_cast<UINT32>(written));
        response.Writer.Next(written);
        return {};
    }

    LX_INT HandleFsync(SpanReader& reader)
    {
        const auto fid = reader.U32();

        const auto file = LookupFid(fid);
        return file->Fsync();
    }

    LX_INT HandleLink(SpanReader& reader)
    {
        const auto dfid = reader.U32();
        const auto fid = reader.U32();
        auto name = reader.Name();

        auto [dir, file] = LookupFidPair(dfid, fid);
        return dir->Link(name, *file);
    }

    Task<LX_INT> HandleRead(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        const auto offset = reader.U64();
        const auto count = reader.U32();

        const auto file = LookupFid(fid);
        response.EnsureSize(MessageType::Rread, count, m_NegotiatedSize);
        auto result = co_await file->Read(offset, response.Writer.Peek(sizeof(UINT32) + count).subspan(sizeof(UINT32)));
        if (!result)
        {
            co_return result.Error();
        }

        response.Writer.U32(result.Get());
        response.Writer.Next(result.Get());
        co_return LX_INT{};
    }

    Task<LX_INT> HandleWrite(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        const auto offset = reader.U64();
        const auto count = reader.U32();
        auto data = reader.Read(count);

        const auto file = LookupFid(fid);
        auto result = co_await file->Write(offset, data);
        if (!result)
        {
            co_return result.Error();
        }

        response.EnsureSize(MessageType::Rwrite, 0, m_NegotiatedSize);
        response.Writer.U32(result.Get());
        co_return LX_INT{};
    }

    LX_INT HandleUnlinkAt(SpanReader& reader)
    {
        const auto fid = reader.U32();
        auto name = reader.Name();
        const auto flags = reader.U32();

        const auto file = LookupFid(fid);
        return file->UnlinkAt(name, flags);
    }

    LX_INT HandleRemove(SpanReader& reader)
    {
        const auto fid = reader.U32();

        const auto file = LookupFid(fid);
        return file->Remove();
    }

    LX_INT HandleRenameAt(SpanReader& reader)
    {
        const auto oldfid = reader.U32();
        auto oldname = reader.Name();
        const auto newfid = reader.U32();
        auto newname = reader.Name();

        auto [olddir, newdir] = LookupFidPair(oldfid, newfid);
        return olddir->RenameAt(oldname, *newdir, newname);
    }

    LX_INT HandleRename(SpanReader& reader)
    {
        const auto oldFid = reader.U32();
        const auto newFid = reader.U32();
        auto newName = reader.Name();

        auto [oldFile, newDir] = LookupFidPair(oldFid, newFid);
        return oldFile->Rename(*newDir, newName);
    }

    LX_INT HandleMkDir(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto name = reader.Name();
        const auto mode = reader.U32();
        const auto gid = reader.U32();

        const auto file = LookupFid(fid);
        auto result = file->MkDir(name, mode, gid);
        if (!result)
        {
            return result.Error();
        }

        response.EnsureSize(MessageType::Rmkdir, 0, m_NegotiatedSize);
        response.Writer.Qid(result.Get());
        return {};
    }

    LX_INT HandleSetAttr(SpanReader& reader)
    {
        StatResult stat{};
        const auto fid = reader.U32();
        const auto valid = reader.U32();
        stat.Mode = reader.U32();
        stat.Uid = reader.U32();
        stat.Gid = reader.U32();
        stat.Size = reader.U64();
        stat.AtimeSec = reader.U64();
        stat.AtimeNsec = reader.U64();
        stat.MtimeSec = reader.U64();
        stat.MtimeNsec = reader.U64();

        const auto file = LookupFid(fid);
        return file->SetAttr(valid, stat);
    }

    LX_INT HandleLock(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto type = reader.U8();
        const auto flags = reader.U32();
        const auto start = reader.U64();
        const auto length = reader.U64();
        const auto procId = reader.U32();
        auto clientId = reader.String();

        const auto file = LookupFid(fid);
        auto status = file->Lock(LockType{type}, flags, start, length, procId, clientId);
        if (!status)
        {
            return status.Error();
        }

        response.EnsureSize(MessageType::Rlock, 0, m_NegotiatedSize);
        response.Writer.U8(static_cast<UINT8>(status.Get()));
        return {};
    }

    LX_INT HandleGetLock(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        auto type = reader.U8();
        const auto start = reader.U64();
        const auto length = reader.U64();
        const auto procId = reader.U32();
        auto clientId = reader.String();

        const auto file = LookupFid(fid);
        auto result = file->GetLock(LockType{type}, start, length, procId, clientId);
        if (!result)
        {
            return result.Error();
        }

        auto [returnType, returnStart, returnLength, returnProcId, returnClientId] = result.Get();
        response.EnsureSize(MessageType::Rgetlock, 0, m_NegotiatedSize);
        response.Writer.U8(static_cast<UINT8>(returnType));
        response.Writer.U64(returnStart);
        response.Writer.U64(returnLength);
        response.Writer.U32(returnProcId);
        response.Writer.String(returnClientId);
        return {};
    }

    LX_INT HandleXattrWalk(SpanReader& reader, MessageResponse& response)
    {
        const auto fid = reader.U32();
        const auto newFid = reader.U32();
        auto name = reader.String();

        const auto entry = LookupFid(fid);
        auto xattr = entry->XattrWalk(std::string{name.data(), static_cast<size_t>(name.size())});
        if (!xattr)
        {
            return xattr.Error();
        }

        auto size = xattr.Get()->GetSize();
        if (!size)
        {
            return size.Error();
        }

        EmplaceFid(newFid, xattr.Get());
        response.EnsureSize(MessageType::Rxattrwalk, 0, m_NegotiatedSize);
        response.Writer.U64(size.Get());
        return {};
    }

    LX_INT HandleXattrCreate(SpanReader& reader)
    {
        const auto fid = reader.U32();
        auto name = reader.String();
        const auto size = reader.U64();
        const auto flags = reader.U32();

        const auto entry = LookupFid(fid);

        const std::string nameString{name.data(), static_cast<size_t>(name.size())};
        auto xattr = entry->XattrCreate(nameString, size, flags);
        if (!xattr)
        {
            return xattr.Error();
        }

        // Unlike xattrwalk, xattrcreate updates the current fid, so replace
        // it.
        std::lock_guard<std::shared_mutex> lock{m_FidsLock};
        const auto iterator = m_Fids.find(fid);
        THROW_UNEXPECTED_IF((iterator == m_Fids.end()) || (iterator->second != entry));
        iterator->second = xattr.Get();
        return {};
    }

    LX_INT HandleAccess(SpanReader& reader)
    {
        if (!m_Use9P2000W)
        {
            return LX_ENOTSUP;
        }

        const auto fid = reader.U32();
        auto flags = reader.U32();
        const auto entry = LookupFid(fid);
        return entry->Access(static_cast<AccessFlags>(flags));
    }

    // Handle the 9P2000.W Twopen message.
    //
    // This message combines the functionality of walk, open, create, mkdir, readlink, and getattr.
    // Certain error conditions (a part of the path could not be found, or a component in the path
    // was not a directory) are reported not using Rlerror, but using Rwopen with an appropriate
    // status code. In this case, the response informs the caller how many components of the path
    // were processed, and returns the attributes of the last successfully walked component.
    //
    // If a symlink is encountered in the path (including as the leaf component), its target is
    // also returned. Whether a symlink as the leaf is treated as an error or success depends on
    // whether OpenSymlink is specified.
    //
    // The return status indicates whether an existing file was opened or a new one was created.
    // If a new file has to be created, this function creates a directory if O_DIRECTORY was
    // specified.
    //
    // Only if the response indicates the status Opened or Created is the "newfid" argument used,
    // and needs to be clunked. With any other status, the client can reuse that fid immediately.
    LX_INT HandleWOpen(SpanReader& reader, MessageResponse& response)
    {
        if (!m_Use9P2000W)
        {
            return LX_ENOTSUP;
        }

        const auto fid = reader.U32();
        const auto newFid = reader.U32();
        auto flags = static_cast<OpenFlags>(reader.U32());
        auto wflags = static_cast<WOpenFlags>(reader.U32());
        const auto mode = reader.U32();
        const auto gid = reader.U32();
        const auto attrMask = reader.U64();
        const auto nameCount = reader.U16();

        const auto entry = LookupFid(fid);
        const auto newFile = entry->Clone();

        bool exists = false;
        bool needOpen = true;
        Qid entryQid = newFile->GetQid();
        if (nameCount > 0)
        {
            // Step 1: Find the parent of the final item.
            for (UINT16 i = 0; i < nameCount - 1; ++i)
            {
                auto qid = newFile->Walk(reader.Name());
                if (!qid)
                {
                    // For ENOENT and ENOTDIR, indicate how many components were processed.
                    switch (qid.Error())
                    {
                    case LX_ENOENT:
                        return WriteWOpenReply(WOpenStatus::ParentNotFound, i, *newFile, attrMask, response);

                    case LX_ENOTDIR:
                        return WriteWOpenReply(WOpenStatus::Stopped, i, *newFile, attrMask, response);

                    default:
                        return qid.Error();
                    }
                }
            }

            auto name = reader.Name();

            // Step 2: Find the item, unless it's an exclusive create.
            int retries;
            for (retries = 0; retries < c_createRetryCount; ++retries)
            {
                if (!WI_AreAllFlagsSet(flags, OpenFlags::Create | OpenFlags::Exclusive))
                {
                    auto qid = newFile->Walk(name);
                    if (!qid)
                    {
                        // For ENOENT (only if not creating) and ENOTDIR, indicate how many components were processed.
                        switch (qid.Error())
                        {
                        case LX_ENOENT:
                            if (!WI_IsFlagSet(flags, OpenFlags::Create))
                            {
                                return WriteWOpenReply(WOpenStatus::NotFound, nameCount - 1, *newFile, attrMask, response);
                            }

                            break;

                        case LX_ENOTDIR:
                            return WriteWOpenReply(WOpenStatus::Stopped, nameCount - 1, *newFile, attrMask, response);

                        default:
                            return qid.Error();
                        }
                    }
                    else
                    {
                        entryQid = *qid;
                        exists = true;
                    }
                }

                // Step 3: Create the item if it didn't exist and the user wants to create it.
                if (!exists && WI_IsFlagSet(flags, OpenFlags::Create))
                {
                    Expected<Qid> qid;

                    // This operation can create a directory if needed.
                    if (WI_IsFlagSet(flags, OpenFlags::Directory))
                    {
                        qid = newFile->MkDir(name, mode, gid);
                    }
                    else
                    {
                        // This will already open the file.
                        needOpen = false;
                        qid = newFile->Create(name, flags | OpenFlags::Exclusive, mode, gid);
                    }

                    if (!qid)
                    {
                        // If this is a non-exclusive create, we tried to find the item above and
                        // then tried to create it exclusively. There is the possibility of a race
                        // if the file got created in between the two calls, so retry if that
                        // happens.
                        //
                        // N.B. A non-exclusive create can't be used directly because the reply must
                        //      indicate whether the file was created or opened.
                        if (qid.Error() == LX_EEXIST && !WI_IsFlagSet(flags, OpenFlags::Exclusive))
                        {
                            needOpen = true;
                            continue;
                        }

                        return qid.Error();
                    }

                    entryQid = *qid;
                }

                break;
            }

            // If a consistent result couldn't be reached, return an error.
            if (retries == c_createRetryCount)
            {
                return LX_EIO;
            }
        }

        // Step 4: Check the file type.
        if (WI_IsFlagSet(wflags, WOpenFlags::NonDirectoryFile) && WI_IsFlagSet(entryQid.Type, QidType::Directory))
        {
            return LX_EISDIR;
        }

        // Check for O_DIRECTORY too in case the open call is skipped below.
        if (WI_IsFlagSet(flags, OpenFlags::Directory) && !WI_IsFlagSet(entryQid.Type, QidType::Directory))
        {
            return LX_ENOTDIR;
        }

        // Step 5: Check for delete access.
        if (WI_IsFlagSet(wflags, WOpenFlags::DeleteAccess))
        {
            auto result = newFile->Access(AccessFlags::Delete);
            if (result < 0)
            {
                return result;
            }
        }

        // Step 6: Check how to handle leaf symlinks.
        if (WI_IsFlagSet(entryQid.Type, QidType::Symlink))
        {
            if (WI_IsFlagSet(wflags, WOpenFlags::OpenSymlink))
            {
                // No need to actually open, but do succeed.
                needOpen = false;
            }
            else
            {
                // Return a stopped status.
                return WriteWOpenReply(WOpenStatus::Stopped, nameCount, *newFile, attrMask, response);
            }
        }

        // Step 7: Open if needed. This is only needed if:
        //         - The file hasn't been opened already by a create; and
        //         - Read/write access is requested to the file; or
        //         - The open will have side effects (it will truncate the file).
        const auto access = (flags & OpenFlags::AccessMask);
        if (needOpen && (access != OpenFlags::NoAccess || WI_IsFlagSet(flags, OpenFlags::Truncate)))
        {
            // If the client specified O_NOACCESS, it means it doesn't want any access check done,
            // but O_NOACCESS actually checks for read/write, so fall back on read-only.
            // Also, directories can't be opened for write so change those to read-only too.
            if ((access == OpenFlags::NoAccess) ||
                (WI_IsFlagSet(entryQid.Type, QidType::Directory) && (access == OpenFlags::WriteOnly || access == OpenFlags::ReadWrite)))
            {
                flags = (flags & ~OpenFlags::AccessMask) | OpenFlags::ReadOnly;
            }

            // Create would've been handled above; don't do it here.
            WI_ClearAllFlags(flags, OpenFlags::Create | OpenFlags::Exclusive);
            auto result = newFile->Open(flags);
            RETURN_ERROR_IF_UNEXPECTED(result);
        }

        // Step 8: Get the attributes and reply
        const auto status = exists ? WOpenStatus::Opened : WOpenStatus::Created;
        auto result = WriteWOpenReply(status, nameCount, *newFile, attrMask, response);
        if (result < 0)
        {
            return result;
        }

        EmplaceFid(newFid, newFile);
        return {};
    }

    // Create a Rwopen message.
    LX_INT WriteWOpenReply(WOpenStatus status, UINT16 walked, Fid& fid, UINT64 mask, MessageResponse& response)
    {
        // Determine the attributes of the last entry found.
        auto stat = fid.GetAttr(mask);
        RETURN_ERROR_IF_UNEXPECTED(stat);

        auto qid = std::get<Qid>(*stat);
        if (WI_IsFlagSet(qid.Type, QidType::Symlink))
        {
            response.EnsureSize(MessageType::Rwopen, LX_PATH_MAX, m_NegotiatedSize);
        }
        else
        {
            response.EnsureSize(MessageType::Rwopen, 0, m_NegotiatedSize);
        }

        response.Writer.U8(static_cast<UINT8>(status));
        response.Writer.U16(walked);
        response.Writer.Qid(qid);

        // If this is a symlink, get the target.
        if (WI_IsFlagSet(qid.Type, QidType::Symlink))
        {
            auto buffer = response.Writer.Peek().subspan(sizeof(UINT16));
            auto charSpan = gsl::span<char>(reinterpret_cast<char*>(buffer.data()), buffer.size());
            auto size = fid.ReadLink(charSpan);
            if (size)
            {
                response.Writer.U16(gsl::narrow_cast<UINT16>(*size));
                response.Writer.Next(*size);
            }
            else
            {
                response.Writer.U16(0);
            }
        }
        else
        {
            response.Writer.U16(0);
        }

        response.Writer.U32(IoUnit());
        util::SpanWriteStatResult(response.Writer, std::get<StatResult>(*stat));
        response.Writer.U64(0); // btime sec (reserved)
        response.Writer.U64(0); // btime nsec (reserved)
        response.Writer.U64(0); // gen (reserved)
        response.Writer.U64(0); // data version (reserved)
        return {};
    }

    // Cancel an outstanding request.
    Task<LX_INT> HandleFlush(SpanReader& reader)
    {
        const auto oldTag = reader.U16();
        std::unique_ptr<RequestInfo> waitRequest;

        {
            std::lock_guard<std::mutex> lock{m_Requests->Lock};

            // Search the list for the specified request.
            for (auto& request : m_Requests->Requests)
            {
                if (request.Tag == oldTag)
                {
                    // A client should not send more than one Tflush on the same request. If it
                    // does, another Tflush request already has ownership and it can't be taken
                    // away. In this case, return success immediately and the client will have to
                    // deal with the result of its broken behavior (but at least the server didn't
                    // crash).
                    if (!request.Cancelled)
                    {
                        // Mark the request cancelled and take ownership of it, so it can be
                        // waited on outside the lock.
                        // N.B. See the destructor of RequestTracker for why this is done this
                        //      way.
                        request.Cancelled = true;
                        waitRequest.reset(&request);
                        break;
                    }
                }
            }
        }

        // Wait until the request completes before sending the Rflush response. This is necessary
        // because the server doesn't support true cancellation, and some messages may modify
        // server state (e.g. Twalk), so the client must receive the response to the real request
        // before it receives the Rflush response.
        if (waitRequest)
        {
            co_await waitRequest->Event;
        }

        co_return 0;
    }

    Task<bool> FillData(UINT32 requiredBytes, CancelToken& token)
    {
        WI_ASSERT(m_RequestData.size() < requiredBytes);

        UINT32 validLength = static_cast<UINT32>(m_RequestData.size());
        if (validLength > 0 && m_RequestData.data() != m_RequestBuffer.data())
        {
            std::copy(m_RequestData.begin(), m_RequestData.end(), m_RequestBuffer.begin());
        }

        while (validLength < requiredBytes)
        {
            size_t count = co_await m_Socket->RecvAsync(gsl::span<gsl::byte>(m_RequestBuffer).subspan(validLength), token);
            if (count == 0)
            {
                break;
            }

            validLength += static_cast<int>(count);
        }

        m_RequestData = gsl::span<gsl::byte>(m_RequestBuffer).subspan(0, validLength);
        co_return validLength >= requiredBytes;
    }

    Task<gsl::span<gsl::byte>> NextMessage(CancelToken& token)
    {
        if (m_RequestData.size() < 4)
        {
            if (!co_await FillData(4, token))
            {
                co_return gsl::span<gsl::byte>{};
            }
        }

        auto messageSize = SpanReader{m_RequestData}.U32();
        THROW_INVALID_IF(messageSize < 7 || messageSize > m_NegotiatedSize);

        if (m_RequestData.size() < messageSize)
        {
            if (!co_await FillData(messageSize, token))
            {
                co_return gsl::span<gsl::byte>{};
            }
        }

        auto message = m_RequestData.subspan(0, messageSize);
        m_RequestData = m_RequestData.subspan(messageSize);
        co_return message;
    }

    // Process a message received from a socket.
    Task<void> ProcessMessage(gsl::span<const gsl::byte> message, CancelToken& sendToken)
    {
        SpanReader reader{message};

        // Utilize a small stack-allocated buffer that's large enough for the largest response
        // without dynamic content (which is Rgetattr). Messages requiring a larger response will
        // allocate a dynamic buffer by calling MessageResponse::EnsureSize.
        // N.B. Message handlers that only return the header (e.g. HandleClunk) don't need to call
        //      EnsureSize since the static buffer is always big enough for that.
        gsl::byte staticBuffer[c_staticBufferSize];
        MessageResponse response{staticBuffer};
        co_await ProcessMessage(reader, response);
        auto m = response.Writer.Result();

        {
            auto lock = co_await m_SocketLock.Lock();

            // Send the response.
            co_await m_Socket->SendAsync(m, sendToken);
        }
    }

    // Process a Plan 9 message, and write the response to the specified buffer.
    Task<void> ProcessMessage(SpanReader& reader, MessageResponse& response)
    {
        LogMessage(reader.Span());
        reader.U32(); // message size, already validated
        auto messageType = reader.U8();
        const auto messageTag = reader.U16();
        const SpanWriter errorWriter{response.Writer};

        LX_INT error;
        try
        {
            error = co_await HandleMessage(static_cast<MessageType>(messageType), reader, response);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            error = util::LinuxErrorFromCaughtException();
        }

        if (error != 0)
        {
            response.Writer = errorWriter;
            response.Writer.U32(static_cast<UINT32>(-error));
            messageType = static_cast<UINT8>(MessageType::Tlerror);
        }

        response.Writer.Header(static_cast<MessageType>(messageType + 1), messageTag);
        LogMessage(response.Writer.Result());
    }

    // Process a message received from virtio.
    void ProcessMessageAsync(std::vector<gsl::byte>&& message, size_t responseSize, HandlerCallback&& callback) override
    {
        // Register the request so Tflush can wait on it if needed.
        const auto tag = SpanReader{gsl::make_span(message).subspan(TagOffset)}.U16();
        RequestTracker request{m_Requests, tag};

        // Process the message in a coroutine. This routine will run synchronously until it hits
        // a suspension point (which it may or may not depending on the message). The coroutine
        // is not awaited here so if it does hit a suspension point the message will be completed
        // asynchronously.
        // N.B. The use of AsyncTask is required because the coroutine is not awaited.
        // N.B. Since this thread is not running the scheduler it will not be used to run other
        //      coroutines if this coroutine hits a suspension point.
        RunAsyncTask(
            [this,
             localMessage = std::move(message),
             localRequest = std::move(request),
             responseSize,
             completionCallback = std::move(callback)]() mutable -> Task<void> {
                std::vector<gsl::byte> responseBuffer;
                try
                {
                    SpanReader reader{localMessage};

                    // Since the response buffer was sized based on the virtio write span, it's not
                    // allowed to reallocate it for a bigger response.
                    responseBuffer.resize(responseSize);
                    MessageResponse response{responseBuffer, false};

                    co_await ProcessMessage(reader, response);
                    responseBuffer.resize(response.Writer.Size());
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION();
                    responseBuffer.clear();
                }

                completionCallback(responseBuffer);
            });
    }

public:
    Task<void> Run(CancelToken& parentToken) noexcept
    {
        Plan9TraceLoggingProvider::AcceptedConnection();
        CancelToken connectionToken(parentToken);
        CancelToken recvToken(connectionToken);
        CancelToken sendToken(connectionToken);
        constexpr size_t maximumMessages = 32; // maximum number of concurrent messages
        AsyncSemaphore messageSemaphore(maximumMessages);
        while (!connectionToken.Cancelled())
        {
            // Only a single read is performed at a time, so no locking is
            // necessary.
            gsl::span<gsl::byte> message{};
            try
            {
                message = co_await NextMessage(recvToken);
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
            }

            if (message.empty())
            {
                break;
            }

            // Register the request so Tflush can wait on it if needed.
            const auto tag = SpanReader{message.subspan(TagOffset)}.U16();
            RequestTracker request{m_Requests, tag};
            co_await messageSemaphore.Acquire(1);

            // Process the message on a separate scheduled coroutine. Receiving
            // messages uses a shared buffer, which can be changed after the
            // next call to NextMessage, so make a copy of the message.
            RunScheduledTask(
                [this,
                 releaseSemaphore = wil::scope_exit([&]() { messageSemaphore.Release(1); }),
                 localMessage = std::vector<gsl::byte>{message.begin(), message.end()},
                 localRequest = std::move(request),
                 &connectionToken,
                 &sendToken]() mutable -> Task<void> {
                    try
                    {
                        co_await ProcessMessage(localMessage, sendToken);
                    }
                    catch (...)
                    {
                        LOG_CAUGHT_EXCEPTION();
                        connectionToken.Cancel();
                    }
                });
        }

        // Wait until all messages are finished.
        connectionToken.Cancel();
        co_await messageSemaphore.Acquire(maximumMessages);
        Plan9TraceLoggingProvider::ConnectionDisconnected();
        co_return;
    }

private:
    std::shared_ptr<Fid> LookupFid(UINT32 fid)
    {
        std::shared_lock<std::shared_mutex> lock{m_FidsLock};
        const auto it = m_Fids.find(fid);
        THROW_UNEXPECTED_IF(it == m_Fids.end());
        return it->second;
    }

    std::pair<std::shared_ptr<Fid>, std::shared_ptr<Fid>> LookupFidPair(UINT32 fid1, UINT32 fid2)
    {
        std::shared_lock<std::shared_mutex> lock{m_FidsLock};
        const auto it1 = m_Fids.find(fid1);
        THROW_UNEXPECTED_IF(it1 == m_Fids.end());
        const auto it2 = m_Fids.find(fid2);
        THROW_UNEXPECTED_IF(it2 == m_Fids.end());
        return {it1->second, it2->second};
    }

    void EmplaceFid(UINT32 fid, std::shared_ptr<Fid> item)
    {
        std::lock_guard<std::shared_mutex> lock{m_FidsLock};
        const auto result = m_Fids.try_emplace(fid, item);
        THROW_INVALID_IF(!result.second);
    }

    // Returns the maximum size of an IO request (0 for no limit).
    static UINT32 IoUnit()
    {
        return 0;
    }

    static constexpr UINT32 MinimumRequestBufferSize = 4096;
    static constexpr UINT32 MaximumRequestBufferSize = 256 * 1024;
    static constexpr UINT32 InitialResponseBufferSize = 64;

    AsyncLock m_SocketLock;
    ISocket* m_Socket{};
    std::shared_mutex m_FidsLock;
    std::map<UINT32, std::shared_ptr<Fid>> m_Fids;
    std::vector<gsl::byte> m_RequestBuffer{MaximumRequestBufferSize};
    gsl::span<gsl::byte> m_RequestData;
    std::shared_ptr<RequestList> m_Requests;
    UINT32 m_NegotiatedSize{InitialResponseBufferSize};
    bool m_Negotiated{false};
    bool m_AllowRenegotiate{false};
    bool m_Use9P2000W{false};
    IShareList& m_ShareList;
};

AsyncTask HandleConnections(ISocket& listen, IShareList& shareList, CancelToken& token, WaitGroup& waitGroup)
{
    std::atomic<size_t> connectionCount{};

    try
    {
        while (!token.Cancelled())
        {
            Plan9TraceLoggingProvider::PreAccept();
            auto client = co_await listen.AcceptAsync(token);
            Plan9TraceLoggingProvider::PostAccept();

            // If the operation was aborted, no socket is returned.
            if (!client)
            {
                Plan9TraceLoggingProvider::OperationAborted();

                token.Cancel();
                break;
            }

            if (connectionCount >= shareList.MaximumConnectionCount())
            {
                Plan9TraceLoggingProvider::TooManyConnections();
                // Terminate the client now so that there is quick feedback
                // that no more connections are allowed.
                client.reset();
            }
            else
            {
                ++connectionCount;
                Plan9TraceLoggingProvider::ClientConnected(connectionCount);

                RunScheduledTask([client = std::move(client), keepAlive = waitGroup.Add(), &connectionCount, &shareList, &token]() -> Task<void> {
                    auto decrementCount = wil::scope_exit([&]() {
                        --connectionCount;
                        Plan9TraceLoggingProvider::ClientDisconnected(connectionCount);
                    });
                    Handler handler{*client, shareList};
                    co_await handler.Run(token);
                });
            }
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        token.Cancel();
    }

    // Wait for the connection tasks to complete.
    co_await waitGroup.Wait();

    WI_ASSERT(connectionCount == 0);
}

// Creates a handler that can be used to process messages without a server socket, for use with
// virtio servers.
std::unique_ptr<IHandler> HandlerFactory::CreateHandler() const
{
    // Since it's not possible to detect a "disconnect" with virtio, allow Tversion to be sent
    // multiple times so the device can be mounted/dismounted more than once without restarting
    // the VM.
    return std::make_unique<Handler>(m_shareList, true);
}

} // namespace p9fs
