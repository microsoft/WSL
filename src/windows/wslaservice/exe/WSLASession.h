
#pragma once

#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASession, IFastRundown>
{
public:
    WSLASession(const WSLA_SESSION_SETTINGS& Settings);
    IFACEMETHOD(GetDisplayName)(LPWSTR* DisplayName);

private:
    std::wstring m_displayName;
};

} // namespace wsl::windows::service::wsla