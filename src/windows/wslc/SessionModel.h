#pragma once

#include <wslservice.h>

namespace wslc::models
{
struct Session
{
    explicit Session(wil::com_ptr<IWSLASession> session) : m_session(std::move(session)) {}
    IWSLASession* Get() const noexcept
    {
        return m_session.get();
    }

private:
    wil::com_ptr<IWSLASession> m_session;
};

struct SessionOptions : public WSLA_SESSION_SETTINGS
{
    static SessionOptions Default();
};
}
