#pragma once
#include "wslservice.h"
#include "hcs.hpp"

namespace wsl::windows::service::lsw {
class DECLSPEC_UUID("0CFC5DC1-B6A7-45FC-8034-3FA9ED73CE30") LSWVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ILSWVirtualMachine, IFastRundown>

{
public:
    LSWVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID Sid);

    void Start();

    IFACEMETHOD(GetState()) override;

private:
    VIRTUAL_MACHINE_SETTINGS m_settings;
    GUID m_vmId{};
    wsl::windows::common::helpers::WindowsVersion m_windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    int m_coldDiscardShiftSize{};
    PSID m_userSid{};

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;
};
} // namespace wsl::windows::service::lsw