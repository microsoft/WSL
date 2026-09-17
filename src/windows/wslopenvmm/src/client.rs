// Copyright (C) Microsoft Corporation. All rights reserved.

use std::collections::BTreeMap;
use std::future::Future;
use std::path::Path;
use std::time::{Duration, Instant};

use parking_lot::Mutex;
use tokio::runtime::Runtime;
use tokio::sync::watch;
use tonic::Request;
use vmservice::{
    ConsommeBackend, CreateVmRequest, DirectBoot, DiskType, ModifyResourceRequest, ModifyType,
    NicConfig, PortConfig, ScsiDisk, VirtioConsoleConfig, VmConfig, vm_client::VmClient,
};
use windows::Win32::Foundation::{
    E_FAIL, E_INVALIDARG, ERROR_ALREADY_EXISTS, ERROR_NOT_FOUND, S_OK,
};
use windows::core::{GUID, HRESULT};

use crate::{af_unix, rpc, vmservice};

#[derive(Debug, PartialEq, Eq)]
enum VmConfigStatus {
    Ready,
    CreationOutcomeUnknown,
}

pub struct VmConfigBuilder {
    config: VmConfig,
    status: VmConfigStatus,
}

pub struct VmConfigHandle {
    builder: Mutex<VmConfigBuilder>,
}

impl VmConfigHandle {
    pub fn new() -> Self {
        crate::diagnostics::ensure_tracing_init();
        Self {
            builder: Mutex::new(VmConfigBuilder::new()),
        }
    }

    pub fn update(&self, operation: impl FnOnce(&mut VmConfigBuilder) -> HRESULT) -> HRESULT {
        operation(&mut self.builder.lock())
    }

    pub fn create_vm(&self, socket_path: String, timeout_ms: u32) -> Result<VmHandle, HRESULT> {
        if timeout_ms == 0 {
            tracing::warn!(target: "wslopenvmm::rpc", "CreateVm requires a nonzero timeout");
            return Err(E_INVALIDARG);
        }
        let timeout = Duration::from_millis(u64::from(timeout_ms));
        let started = Instant::now();
        let deadline = started + timeout;
        tracing::info!(target: "wslopenvmm::rpc", "CreateVm starting with timeout {timeout_ms} ms");
        let Some(mut builder) = self.builder.try_lock_until(deadline) else {
            tracing::warn!(target: "wslopenvmm::rpc", "CreateVm timed out waiting for the configuration lock; no request sent");
            return Err(rpc::TIMEOUT);
        };
        let result = builder.create_vm(socket_path, timeout, deadline);
        match &result {
            Ok(_) => {
                tracing::info!(target: "wslopenvmm::rpc", "CreateVm completed in {:?}", started.elapsed())
            }
            Err(error) => tracing::warn!(
                target: "wslopenvmm::rpc",
                "CreateVm failed with HRESULT {:#010x} after {:?}; configuration status: {:?}",
                error.0, started.elapsed(), builder.status
            ),
        }
        result
    }
}

pub struct VmHandle {
    inner: Mutex<VmHandleInner>,
    timeout: Duration,
    cancellation: watch::Sender<bool>,
}

struct VmHandleInner {
    runtime: Runtime,
    client: VmClient<tonic::transport::Channel>,
    shares: BTreeMap<String, String>,
    status: VmStatus,
}

#[derive(Debug, PartialEq, Eq)]
enum VmStatus {
    Active,
    RecoveryRequired,
}

impl VmConfigBuilder {
    fn new() -> Self {
        Self {
            config: VmConfig::default(),
            status: VmConfigStatus::Ready,
        }
    }

    fn create_vm(
        &mut self,
        socket_path: String,
        timeout: Duration,
        deadline: Instant,
    ) -> Result<VmHandle, HRESULT> {
        if self.status == VmConfigStatus::CreationOutcomeUnknown {
            tracing::warn!(target: "wslopenvmm::rpc", "CreateVm rejected: previous creation outcome is unknown");
            return Err(rpc::INVALID_STATE);
        }
        let runtime = match tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
        {
            Ok(runtime) => runtime,
            Err(_) => {
                tracing::error!(target: "wslopenvmm::rpc", "CreateVm could not initialize the RPC runtime");
                return Err(E_FAIL);
            }
        };
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            tracing::warn!(target: "wslopenvmm::rpc", "CreateVm deadline expired before connecting");
            return Err(rpc::TIMEOUT);
        }
        let channel = match runtime
            .block_on(af_unix::connect_channel(socket_path.into(), remaining))
        {
            Ok(channel) => channel,
            Err(error) => {
                let result = rpc::io_error_to_hresult(&error);
                tracing::warn!(target: "wslopenvmm::rpc", "CreateVm connection failed with HRESULT {:#010x}; no creation request sent", result.0);
                return Err(result);
            }
        };
        let mut client = VmClient::new(channel);
        let request = CreateVmRequest {
            config: Some(self.config.clone()),
            log_id: String::new(),
        };
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            tracing::warn!(target: "wslopenvmm::rpc", "CreateVm deadline expired before dispatch");
            return Err(rpc::TIMEOUT);
        }
        match runtime.block_on(rpc::execute(
            deadline,
            None,
            client.create_vm(request_with_timeout(request, remaining)),
        )) {
            Ok(_) => Ok(VmHandle::new(runtime, client, timeout)),
            Err(error) => {
                if error.uncertain {
                    self.status = VmConfigStatus::CreationOutcomeUnknown;
                    tracing::warn!(target: "wslopenvmm::rpc", "CreateVm outcome is unknown; discard the configuration and process before retrying");
                }
                Err(error.result)
            }
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
        self.config.memory_config.get_or_insert_default().memory_mb = memory_mb;
        S_OK
    }

    pub fn set_processor_count(&mut self, count: u32) -> HRESULT {
        self.config
            .processor_config
            .get_or_insert_default()
            .processor_count = count;
        S_OK
    }

    pub fn set_hvsocket_path(&mut self, path: String) -> HRESULT {
        self.config.hvsocket_config.get_or_insert_default().path = path;
        S_OK
    }

    pub fn add_boot_disk(
        &mut self,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        self.config
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
        self.config
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
        self.config
            .serial_config
            .get_or_insert_default()
            .ports
            .push(vmservice::serial_config::Config {
                port,
                socket_path: pipe_name,
                connect: true,
            });
        S_OK
    }

    pub fn set_virtio_console_path(&mut self, path: String) -> HRESULT {
        self.config
            .devices_config
            .get_or_insert_default()
            .virtio_console = Some(VirtioConsoleConfig {
            socket_path: path,
            connect: true,
        });
        S_OK
    }

    fn direct_boot(&mut self) -> &mut DirectBoot {
        let boot_config = self.config.boot_config.get_or_insert_with(|| {
            vmservice::vm_config::BootConfig::DirectBoot(DirectBoot::default())
        });
        match boot_config {
            vmservice::vm_config::BootConfig::DirectBoot(direct_boot) => direct_boot,
            _ => unreachable!("boot config was not set to DirectBoot"),
        }
    }
}

impl VmHandle {
    fn new(
        runtime: Runtime,
        client: VmClient<tonic::transport::Channel>,
        timeout: Duration,
    ) -> Self {
        Self {
            inner: Mutex::new(VmHandleInner {
                runtime,
                client,
                shares: BTreeMap::new(),
                status: VmStatus::Active,
            }),
            timeout,
            cancellation: watch::channel(false).0,
        }
    }

    pub fn cancel_requests(&self) -> HRESULT {
        self.cancellation.send_replace(true);
        tracing::info!(target: "wslopenvmm::rpc", "VM request cancellation signalled");
        S_OK
    }

    fn with_operation(
        &self,
        name: &'static str,
        cleanup: bool,
        operation: impl FnOnce(&mut VmHandleInner, Instant, Option<watch::Receiver<bool>>) -> HRESULT,
    ) -> HRESULT {
        let started = Instant::now();
        let deadline = started + self.timeout;
        tracing::info!(target: "wslopenvmm::rpc", "{name} starting");
        let Some(mut inner) = self.inner.try_lock_until(deadline) else {
            // No request was sent, so lock contention alone does not invalidate the VM.
            tracing::warn!(target: "wslopenvmm::rpc", "{name} timed out waiting for the VM lock; no request sent");
            return rpc::TIMEOUT;
        };
        if *self.cancellation.borrow() {
            inner.status = VmStatus::RecoveryRequired;
        }
        if !cleanup && inner.status == VmStatus::RecoveryRequired {
            tracing::warn!(target: "wslopenvmm::rpc", "{name} rejected: VM requires recovery");
            return rpc::INVALID_STATE;
        }
        let cancellation = if cleanup {
            None
        } else {
            Some(self.cancellation.subscribe())
        };
        let result = operation(&mut inner, deadline, cancellation);
        // Cancellation can race a successful response, including during cleanup.
        if *self.cancellation.borrow() {
            inner.status = VmStatus::RecoveryRequired;
        }
        if result.is_err() {
            tracing::warn!(
                target: "wslopenvmm::rpc",
                "{name} failed with HRESULT {:#010x} after {:?}; VM status: {:?}",
                result.0, started.elapsed(), inner.status
            );
        } else {
            tracing::info!(
                target: "wslopenvmm::rpc",
                "{name} completed in {:?}; VM status: {:?}",
                started.elapsed(), inner.status
            );
        }
        result
    }

    pub fn resume_vm(&self) -> HRESULT {
        self.with_operation("ResumeVm", false, |inner, deadline, cancellation| {
            self.rpc(
                inner,
                deadline,
                cancellation,
                (),
                |mut client, request| async move { client.resume_vm(request).await },
            )
        })
    }

    pub fn teardown_vm(&self) -> HRESULT {
        self.with_operation("TeardownVm", true, |inner, deadline, cancellation| {
            let result = self.rpc(
                inner,
                deadline,
                cancellation,
                (),
                |mut client, request| async move { client.teardown_vm(request).await },
            );
            if result.is_ok() {
                inner.shares.clear();
            }
            result
        })
    }

    pub fn quit(&self) -> HRESULT {
        self.with_operation("Quit", true, |inner, deadline, cancellation| {
            self.rpc(
                inner,
                deadline,
                cancellation,
                (),
                |mut client, request| async move { client.quit(request).await },
            )
        })
    }

    pub fn attach_scsi_disk(
        &self,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        self.modify_disk(ModifyType::Add, controller, lun, host_path, read_only)
    }

    pub fn detach_scsi_disk(&self, controller: u32, lun: u32) -> HRESULT {
        self.modify_disk(ModifyType::Remove, controller, lun, String::new(), false)
    }

    pub fn add_consomme_nic(&self, nic_id: String, mac_address: String, cidr: String) -> HRESULT {
        self.with_operation("AddConsommeNic", false, |inner, deadline, cancellation| {
            let request = ModifyResourceRequest {
                r#type: ModifyType::Add as i32,
                resource: Some(vmservice::modify_resource_request::Resource::NicConfig(
                    NicConfig {
                        nic_id,
                        mac_address,
                        backend: Some(vmservice::nic_config::Backend::Consomme(ConsommeBackend {
                            cidr,
                            ports: Vec::new(),
                        })),
                        ..Default::default()
                    },
                )),
            };
            self.rpc(
                inner,
                deadline,
                cancellation,
                request,
                |mut client, request| async move { client.modify_resource(request).await },
            )
        })
    }

    pub fn bind_port(
        &self,
        nic_id: String,
        host_port: u16,
        guest_port: u16,
        tcp: bool,
        host_address: String,
    ) -> HRESULT {
        self.modify_port(
            ModifyType::Update,
            nic_id,
            host_port,
            guest_port,
            tcp,
            host_address,
        )
    }

    pub fn unbind_port(
        &self,
        nic_id: String,
        host_port: u16,
        guest_port: u16,
        tcp: bool,
        host_address: String,
    ) -> HRESULT {
        self.modify_port(
            ModifyType::Remove,
            nic_id,
            host_port,
            guest_port,
            tcp,
            host_address,
        )
    }

    pub fn add_share(&self, tag: String, host_path: String, read_only: bool) -> HRESULT {
        self.with_operation("AddShare", false, |inner, deadline, cancellation| {
            if inner.shares.contains_key(&tag) {
                return HRESULT::from_win32(ERROR_ALREADY_EXISTS.0);
            }
            let instance_id = match GUID::new() {
                Ok(instance_id) => format!("{instance_id:?}"),
                Err(error) => return error.code(),
            };
            let request = vmservice::AddVpciDeviceRequest {
                instance_id: instance_id.clone(),
                device: Some(vmservice::PcieDeviceKind {
                    kind: Some(vmservice::pcie_device_kind::Kind::Virtio(
                        vmservice::VirtioDevice {
                            kind: Some(vmservice::virtio_device::Kind::Fs(vmservice::VirtioFs {
                                tag: tag.clone(),
                                root_path: host_path,
                                read_only,
                            })),
                        },
                    )),
                }),
            };
            let result = self.rpc(
                inner,
                deadline,
                cancellation,
                request,
                |mut client, request| async move { client.add_vpci_device(request).await },
            );
            if result.is_ok() {
                inner.shares.insert(tag, instance_id);
            }
            result
        })
    }

    pub fn remove_share(&self, tag: &str) -> HRESULT {
        self.with_operation("RemoveShare", false, |inner, deadline, cancellation| {
            let Some(instance_id) = inner.shares.get(tag) else {
                return HRESULT::from_win32(ERROR_NOT_FOUND.0);
            };
            let request = vmservice::RemoveVpciDeviceRequest {
                instance_id: instance_id.clone(),
            };
            let result = self.rpc(
                inner,
                deadline,
                cancellation,
                request,
                |mut client, request| async move { client.remove_vpci_device(request).await },
            );
            if result.is_ok() {
                inner.shares.remove(tag);
            }
            result
        })
    }

    fn rpc<T, F, R>(
        &self,
        inner: &mut VmHandleInner,
        deadline: Instant,
        cancellation: Option<watch::Receiver<bool>>,
        message: T,
        operation: F,
    ) -> HRESULT
    where
        F: FnOnce(VmClient<tonic::transport::Channel>, Request<T>) -> R,
        R: Future<Output = Result<tonic::Response<()>, tonic::Status>>,
    {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            tracing::warn!(target: "wslopenvmm::rpc", "RPC deadline expired before dispatch; no request sent");
            return rpc::TIMEOUT;
        }
        let result = inner.runtime.block_on(rpc::execute(
            deadline,
            cancellation,
            operation(
                inner.client.clone(),
                request_with_timeout(message, remaining),
            ),
        ));
        match result {
            Ok(_) => S_OK,
            Err(error) => {
                if error.uncertain {
                    inner.status = VmStatus::RecoveryRequired;
                    tracing::warn!(target: "wslopenvmm::rpc", "Uncertain RPC outcome; VM requires teardown and recreation");
                }
                error.result
            }
        }
    }

    fn modify_disk(
        &self,
        modify_type: ModifyType,
        controller: u32,
        lun: u32,
        host_path: String,
        read_only: bool,
    ) -> HRESULT {
        let name = if modify_type == ModifyType::Add {
            "AttachScsiDisk"
        } else {
            "DetachScsiDisk"
        };
        self.with_operation(name, false, |inner, deadline, cancellation| {
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
            self.rpc(
                inner,
                deadline,
                cancellation,
                request,
                |mut client, request| async move { client.modify_resource(request).await },
            )
        })
    }

    fn modify_port(
        &self,
        modify_type: ModifyType,
        nic_id: String,
        host_port: u16,
        guest_port: u16,
        tcp: bool,
        host_address: String,
    ) -> HRESULT {
        let name = if modify_type == ModifyType::Update {
            "BindPort"
        } else {
            "UnbindPort"
        };
        self.with_operation(name, false, |inner, deadline, cancellation| {
            let protocol = if tcp {
                vmservice::IpProtocol::Tcp
            } else {
                vmservice::IpProtocol::Udp
            };
            let request = ModifyResourceRequest {
                r#type: modify_type as i32,
                resource: Some(vmservice::modify_resource_request::Resource::NicConfig(
                    NicConfig {
                        nic_id,
                        backend: Some(vmservice::nic_config::Backend::Consomme(ConsommeBackend {
                            cidr: String::new(),
                            ports: vec![PortConfig {
                                host_port: u32::from(host_port),
                                guest_port: u32::from(guest_port),
                                protocol: protocol as i32,
                                host_address,
                            }],
                        })),
                        ..Default::default()
                    },
                )),
            };
            self.rpc(
                inner,
                deadline,
                cancellation,
                request,
                |mut client, request| async move { client.modify_resource(request).await },
            )
        })
    }
}

fn request_with_timeout<T>(message: T, timeout: Duration) -> Request<T> {
    let mut request = Request::new(message);
    request.set_timeout(timeout);
    request
}

fn disk_type(path: &str) -> i32 {
    if Path::new(path)
        .extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("vhdx"))
    {
        DiskType::ScsiDiskTypeVhdx as i32
    } else {
        DiskType::ScsiDiskTypeVhd1 as i32
    }
}
