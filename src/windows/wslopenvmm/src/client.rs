// Copyright (C) Microsoft Corporation. All rights reserved.

use std::time::Duration;

use tokio::runtime::Runtime;
use tonic::Request;
use vmservice::{
    ConsommeBackend, CreateVmRequest, DirectBoot, DiskType, ModifyResourceRequest, ModifyType,
    NicConfig, PortConfig, ScsiDisk, VirtioConsoleConfig, VmConfig, vm_client::VmClient,
};
use windows::Win32::Foundation::{E_FAIL, ERROR_CONNECTION_ABORTED, S_OK, WAIT_TIMEOUT};
use windows::core::HRESULT;

use crate::{named_pipe, vmservice};

pub struct VmConfigBuilder {
    inner: VmConfig,
}

pub struct VmHandle {
    inner: VmHandleInner,
}

struct VmHandleInner {
    runtime: Runtime,
    client: VmClient<tonic::transport::Channel>,
    timeout: Duration,
}

impl VmConfigBuilder {
    pub fn new() -> Self {
        Self {
            inner: VmConfig::default(),
        }
    }

    pub fn create_vm(&self, pipe_name: String, timeout_ms: u32) -> Result<VmHandle, HRESULT> {
        let runtime = match tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
        {
            Ok(runtime) => runtime,
            Err(_) => return Err(E_FAIL),
        };
        let timeout = Duration::from_millis(u64::from(timeout_ms));
        let channel = match runtime.block_on(named_pipe::connect_channel(pipe_name, timeout)) {
            Ok(channel) => channel,
            Err(error) if error.to_string().contains("timed out") => {
                return Err(HRESULT::from_win32(WAIT_TIMEOUT.0));
            }
            Err(_) => return Err(HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0)),
        };
        let mut client = VmClient::new(channel);
        let request = CreateVmRequest {
            config: Some(self.inner.clone()),
            log_id: String::new(),
        };
        match runtime.block_on(async {
            let mut request = Request::new(request);
            request.set_timeout(timeout);
            client.create_vm(request).await
        }) {
            Ok(_) => Ok(VmHandle {
                inner: VmHandleInner {
                    runtime,
                    client,
                    timeout,
                },
            }),
            Err(error) => Err(rpc_status_to_hresult(error)),
        }
    }

    pub fn set_kernel_path(&mut self, path: String) -> HRESULT {
        self.direct_boot().kernel_path = path;
        S_OK
    }

    pub fn set_initrd_path(&mut self, path: String) -> HRESULT {
        self.direct_boot().initrd_path = path;
        S_OK
    }

    pub fn set_kernel_cmdline(&mut self, command_line: String) -> HRESULT {
        self.direct_boot().kernel_cmdline = command_line;
        S_OK
    }

    pub fn set_memory_mb(&mut self, memory_mb: u64) -> HRESULT {
        self.inner.memory_config.get_or_insert_default().memory_mb = memory_mb;
        S_OK
    }

    pub fn set_processor_count(&mut self, count: u32) -> HRESULT {
        self.inner
            .processor_config
            .get_or_insert_default()
            .processor_count = count;
        S_OK
    }

    pub fn set_hvsocket_path(&mut self, path: String) -> HRESULT {
        self.inner.hvsocket_config.get_or_insert_default().path = path;
        S_OK
    }

    pub fn add_boot_disk(
        &mut self,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        self.inner
            .devices_config
            .get_or_insert_default()
            .scsi_disks
            .push(ScsiDisk {
                controller,
                lun,
                r#type: disk_type(&host_path),
                host_path,
                read_only,
            });
        S_OK
    }

    pub fn set_consomme_nic(&mut self, nic_id: String, mac_address: String) -> HRESULT {
        self.inner
            .devices_config
            .get_or_insert_default()
            .nic_config
            .push(NicConfig {
                nic_id,
                mac_address,
                backend: Some(vmservice::nic_config::Backend::Consomme(
                    ConsommeBackend::default(),
                )),
                ..Default::default()
            });
        S_OK
    }

    pub fn add_serial_port(&mut self, port: u32, pipe_name: String) -> HRESULT {
        self.inner.serial_config.get_or_insert_default().ports.push(
            vmservice::serial_config::Config {
                port,
                socket_path: pipe_name,
                connect: true,
            },
        );
        S_OK
    }

    pub fn set_virtio_console_path(&mut self, path: String) -> HRESULT {
        self.inner
            .devices_config
            .get_or_insert_default()
            .virtio_console = Some(VirtioConsoleConfig {
            socket_path: path,
            connect: true,
        });
        S_OK
    }

    fn direct_boot(&mut self) -> &mut DirectBoot {
        let boot_config = self.inner.boot_config.get_or_insert_with(|| {
            vmservice::vm_config::BootConfig::DirectBoot(DirectBoot::default())
        });
        match boot_config {
            vmservice::vm_config::BootConfig::DirectBoot(direct_boot) => direct_boot,
            _ => unreachable!("boot config was not set to DirectBoot"),
        }
    }
}

impl VmHandle {
    pub fn resume_vm(&mut self) -> HRESULT {
        self.empty_rpc(|client, request| Box::pin(async move { client.resume_vm(request).await }))
    }

    pub fn teardown_vm(&mut self) -> HRESULT {
        self.empty_rpc(|client, request| Box::pin(async move { client.teardown_vm(request).await }))
    }

    pub fn quit(&mut self) -> HRESULT {
        self.empty_rpc(|client, request| Box::pin(async move { client.quit(request).await }))
    }

    pub fn attach_scsi_disk(
        &mut self,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        self.modify_disk(ModifyType::Add, controller, lun, host_path, read_only)
    }

    pub fn detach_scsi_disk(&mut self, controller: u32, lun: u32) -> HRESULT {
        self.modify_disk(ModifyType::Remove, controller, lun, String::new(), false)
    }

    pub fn bind_port(&mut self, host_port: u16, guest_port: u16, tcp: bool) -> HRESULT {
        self.modify_port(ModifyType::Add, host_port, guest_port, tcp)
    }

    pub fn unbind_port(&mut self, host_port: u16, guest_port: u16, tcp: bool) -> HRESULT {
        self.modify_port(ModifyType::Remove, host_port, guest_port, tcp)
    }

    fn empty_rpc<F>(&mut self, operation: F) -> HRESULT
    where
        F: for<'a> FnOnce(
            &'a mut VmClient<tonic::transport::Channel>,
            Request<()>,
        ) -> std::pin::Pin<
            Box<dyn std::future::Future<Output = Result<tonic::Response<()>, tonic::Status>> + 'a>,
        >,
    {
        match self.inner.runtime.block_on(operation(
            &mut self.inner.client,
            request_with_timeout((), self.inner.timeout),
        )) {
            Ok(_) => S_OK,
            Err(error) => rpc_status_to_hresult(error),
        }
    }

    fn modify_disk(
        &mut self,
        modify_type: ModifyType,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        let request = ModifyResourceRequest {
            r#type: modify_type as i32,
            resource: Some(vmservice::modify_resource_request::Resource::ScsiDisk(
                ScsiDisk {
                    controller,
                    lun,
                    r#type: disk_type(&host_path),
                    host_path,
                    read_only,
                },
            )),
        };
        self.modify_resource(request)
    }

    fn modify_port(
        &mut self,
        modify_type: ModifyType,
        host_port: u16,
        guest_port: u16,
        tcp: bool,
    ) -> HRESULT {
        let protocol = if tcp {
            vmservice::IpProtocol::Tcp
        } else {
            vmservice::IpProtocol::Udp
        };
        let request = ModifyResourceRequest {
            r#type: modify_type as i32,
            resource: Some(vmservice::modify_resource_request::Resource::NicConfig(
                NicConfig {
                    backend: Some(vmservice::nic_config::Backend::Consomme(ConsommeBackend {
                        cidr: String::new(),
                        ports: vec![PortConfig {
                            host_port: u32::from(host_port),
                            guest_port: u32::from(guest_port),
                            protocol: protocol as i32,
                        }],
                    })),
                    ..Default::default()
                },
            )),
        };
        self.modify_resource(request)
    }

    fn modify_resource(&mut self, request: ModifyResourceRequest) -> HRESULT {
        match self.inner.runtime.block_on(
            self.inner
                .client
                .modify_resource(request_with_timeout(request, self.inner.timeout)),
        ) {
            Ok(_) => S_OK,
            Err(error) => rpc_status_to_hresult(error),
        }
    }
}

fn request_with_timeout<T>(message: T, timeout: Duration) -> Request<T> {
    let mut request = Request::new(message);
    request.set_timeout(timeout);
    request
}

fn disk_type(path: &str) -> i32 {
    if path.ends_with(".vhdx") || path.ends_with(".VHDX") {
        DiskType::ScsiDiskTypeVhdx as i32
    } else {
        DiskType::ScsiDiskTypeVhd1 as i32
    }
}

fn rpc_status_to_hresult(error: tonic::Status) -> HRESULT {
    if error.code() == tonic::Code::DeadlineExceeded {
        HRESULT::from_win32(WAIT_TIMEOUT.0)
    } else {
        HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0)
    }
}
