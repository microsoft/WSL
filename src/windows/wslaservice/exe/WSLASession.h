/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.h

Abstract:

    TODO

--*/
#pragma once
#include "wslaservice.h"
#include "INetworkingEngine.h"
#include "hcs.hpp"
#include "Dmesg.h"
#include "WSLAApi.h"

namespace wsl::windows::service::wsla {


class WSLASessionImpl
{
public:
    WSLASessionImpl();
    WSLASessionImpl(WSLASessionImpl&&) = default;
    WSLASessionImpl& operator=(WSLASessionImpl&&) = default;

    ~WSLASessionImpl();


private:
};

class DECLSPEC_UUID("0CFC5DC1-B6A7-45FC-8034-3FA9ED73CE30") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASession, IFastRundown>

{
public:
    WSLASession(std::weak_ptr<WSLASessionImpl>&& Session);
    WSLASession(const WSLASession&) = delete;
    WSLASession& operator=(const WSLASession&) = delete;

    void Start();
    void OnSessionTerminating();

private:
 
    std::recursive_mutex m_lock;
    WSLASessionImpl* m_session;
};
} // namespace wsl::windows::service::wsla