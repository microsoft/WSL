// Copyright (C) Microsoft Corporation. All rights reserved.

#![expect(clippy::missing_safety_doc)]

mod client;
pub mod named_pipe;

pub mod vmservice {
    tonic::include_proto!("vmservice");
}

use client::{VmConfigBuilder, VmHandle};
use parking_lot::Mutex;
use windows::Win32::Foundation::{E_INVALIDARG, E_POINTER, S_OK};
use windows::core::{HRESULT, PCWSTR};

pub struct WslOpenVmmConfig(Mutex<VmConfigBuilder>);
pub struct WslOpenVmmVm(Mutex<VmHandle>);

unsafe fn string_from_wide(value: *const u16) -> Result<String, HRESULT> {
    if value.is_null() {
        return Err(E_POINTER);
    }

    unsafe { PCWSTR(value).to_string() }.map_err(|_| E_INVALIDARG)
}

unsafe fn with_config(
    handle: *mut WslOpenVmmConfig,
    operation: impl FnOnce(&mut VmConfigBuilder) -> HRESULT,
) -> HRESULT {
    let Some(config) = (unsafe { handle.as_ref() }) else {
        return E_POINTER;
    };
    operation(&mut config.0.lock())
}

unsafe fn with_vm(
    handle: *mut WslOpenVmmVm,
    operation: impl FnOnce(&mut VmHandle) -> HRESULT,
) -> HRESULT {
    let Some(vm) = (unsafe { handle.as_ref() }) else {
        return E_POINTER;
    };
    operation(&mut vm.0.lock())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmCreateConfig(config: *mut *mut WslOpenVmmConfig) -> i32 {
    if config.is_null() {
        return E_POINTER.0;
    }

    unsafe {
        *config = Box::into_raw(Box::new(WslOpenVmmConfig(Mutex::new(
            VmConfigBuilder::new(),
        ))));
    }
    S_OK.0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmDestroyConfig(config: *mut WslOpenVmmConfig) {
    if !config.is_null() {
        unsafe { drop(Box::from_raw(config)) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetKernelPath(
    config: *mut WslOpenVmmConfig,
    path: *const u16,
) -> i32 {
    let path = match unsafe { string_from_wide(path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.set_kernel_path(path)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetInitrdPath(
    config: *mut WslOpenVmmConfig,
    path: *const u16,
) -> i32 {
    let path = match unsafe { string_from_wide(path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.set_initrd_path(path)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetKernelCmdLine(
    config: *mut WslOpenVmmConfig,
    command_line: *const u16,
) -> i32 {
    let command_line = match unsafe { string_from_wide(command_line) } {
        Ok(value) => value,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.set_kernel_cmdline(command_line)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetMemoryMb(
    config: *mut WslOpenVmmConfig,
    memory_mb: u64,
) -> i32 {
    unsafe { with_config(config, |config| config.set_memory_mb(memory_mb)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetProcessorCount(
    config: *mut WslOpenVmmConfig,
    count: u32,
) -> i32 {
    unsafe { with_config(config, |config| config.set_processor_count(count)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetHvSocketPath(
    config: *mut WslOpenVmmConfig,
    path: *const u16,
) -> i32 {
    let path = match unsafe { string_from_wide(path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.set_hvsocket_path(path)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigAddBootDisk(
    config: *mut WslOpenVmmConfig,
    controller: u32,
    lun: u32,
    host_path: *const u16,
    read_only: i32,
) -> i32 {
    let host_path = match unsafe { string_from_wide(host_path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe {
        with_config(config, |config| {
            config.add_boot_disk(controller, lun, host_path, read_only != 0)
        })
        .0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetConsommeNic(
    config: *mut WslOpenVmmConfig,
    nic_id: *const u16,
    mac_address: *const u16,
) -> i32 {
    let nic_id = match unsafe { string_from_wide(nic_id) } {
        Ok(value) => value,
        Err(error) => return error.0,
    };
    let mac_address = match unsafe { string_from_wide(mac_address) } {
        Ok(value) => value,
        Err(error) => return error.0,
    };
    unsafe {
        with_config(config, |config| {
            config.set_consomme_nic(nic_id, mac_address)
        })
        .0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigAddSerialPort(
    config: *mut WslOpenVmmConfig,
    port: u32,
    pipe_name: *const u16,
) -> i32 {
    let pipe_name = match unsafe { string_from_wide(pipe_name) } {
        Ok(value) => value,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.add_serial_port(port, pipe_name)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmConfigSetVirtioConsolePath(
    config: *mut WslOpenVmmConfig,
    path: *const u16,
) -> i32 {
    let path = match unsafe { string_from_wide(path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe { with_config(config, |config| config.set_virtio_console_path(path)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmCreateVm(
    config: *mut *mut WslOpenVmmConfig,
    pipe_name: *const u16,
    timeout_ms: u32,
    vm: *mut *mut WslOpenVmmVm,
) -> i32 {
    if vm.is_null() {
        return E_POINTER.0;
    }
    unsafe { *vm = std::ptr::null_mut() };
    if config.is_null() {
        return E_POINTER.0;
    }
    let config_handle = unsafe { *config };
    if config_handle.is_null() {
        return E_POINTER.0;
    }
    let pipe_name = match unsafe { string_from_wide(pipe_name) } {
        Ok(pipe_name) => pipe_name,
        Err(error) => return error.0,
    };

    let config_box = unsafe { Box::from_raw(config_handle) };
    let result = config_box.0.lock().create_vm(pipe_name, timeout_ms);
    let client = match result {
        Ok(client) => client,
        Err(error) => {
            unsafe {
                *config = Box::into_raw(config_box);
            }
            return error.0;
        }
    };

    unsafe {
        *config = std::ptr::null_mut();
        *vm = Box::into_raw(Box::new(WslOpenVmmVm(Mutex::new(client))));
    }
    S_OK.0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmDestroyVm(vm: *mut WslOpenVmmVm) {
    if !vm.is_null() {
        unsafe { drop(Box::from_raw(vm)) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmResume(vm: *mut WslOpenVmmVm) -> i32 {
    unsafe { with_vm(vm, VmHandle::resume_vm).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmTeardown(vm: *mut WslOpenVmmVm) -> i32 {
    unsafe { with_vm(vm, VmHandle::teardown_vm).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmQuit(vm: *mut WslOpenVmmVm) -> i32 {
    unsafe { with_vm(vm, VmHandle::quit).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmAttachScsiDisk(
    vm: *mut WslOpenVmmVm,
    controller: u32,
    lun: u32,
    host_path: *const u16,
    read_only: i32,
) -> i32 {
    let host_path = match unsafe { string_from_wide(host_path) } {
        Ok(path) => path,
        Err(error) => return error.0,
    };
    unsafe {
        with_vm(vm, |vm| {
            vm.attach_scsi_disk(controller, lun, host_path, read_only != 0)
        })
        .0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmDetachScsiDisk(
    vm: *mut WslOpenVmmVm,
    controller: u32,
    lun: u32,
) -> i32 {
    unsafe { with_vm(vm, |vm| vm.detach_scsi_disk(controller, lun)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmBindPort(
    vm: *mut WslOpenVmmVm,
    host_port: u16,
    guest_port: u16,
    tcp: i32,
    _: i32,
) -> i32 {
    unsafe { with_vm(vm, |vm| vm.bind_port(host_port, guest_port, tcp != 0)).0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn WslOpenVmmVmUnbindPort(
    vm: *mut WslOpenVmmVm,
    host_port: u16,
    guest_port: u16,
    tcp: i32,
    _: i32,
) -> i32 {
    unsafe { with_vm(vm, |vm| vm.unbind_port(host_port, guest_port, tcp != 0)).0 }
}
