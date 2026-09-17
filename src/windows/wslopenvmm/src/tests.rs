// Copyright (C) Microsoft Corporation. All rights reserved.

//! DLL tests and fixtures. Queue RPC expectations on `MockVmService`, then use
//! `ConfigFixture::create_vm` with `MockServerFixture::socket_path` to obtain a
//! `VmFixture`. Handles exercise the real DLL ABI; only the OpenVMM peer is mocked.
//! The server verifies that expectations were consumed when it is dropped.

#![allow(dead_code)] // Some fixture capabilities are reserved for additional scenarios.

use std::collections::VecDeque;
use std::io;
use std::path::PathBuf;
use std::ptr;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use parking_lot::{Condvar, Mutex};
use socket2::{Domain, SockAddr, Socket, Type};
use tokio::sync::oneshot;
use tonic::{Request, Response, Status};
use windows::Win32::Foundation::S_OK;
use windows::core::{GUID, HRESULT};

use crate::vmservice;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RpcMethod {
    CreateVm,
    ResumeVm,
    TeardownVm,
    Quit,
    ModifyResource,
    AddVpciDevice,
    RemoveVpciDevice,
}

#[derive(Clone, Debug, PartialEq)]
enum RpcRequest {
    CreateVm(Box<vmservice::CreateVmRequest>),
    ResumeVm,
    TeardownVm,
    Quit,
    ModifyResource(Box<vmservice::ModifyResourceRequest>),
    AddVpciDevice(Box<vmservice::AddVpciDeviceRequest>),
    RemoveVpciDevice(vmservice::RemoveVpciDeviceRequest),
}

impl RpcRequest {
    fn method(&self) -> RpcMethod {
        match self {
            Self::CreateVm(_) => RpcMethod::CreateVm,
            Self::ResumeVm => RpcMethod::ResumeVm,
            Self::TeardownVm => RpcMethod::TeardownVm,
            Self::Quit => RpcMethod::Quit,
            Self::ModifyResource(_) => RpcMethod::ModifyResource,
            Self::AddVpciDevice(_) => RpcMethod::AddVpciDevice,
            Self::RemoveVpciDevice(_) => RpcMethod::RemoveVpciDevice,
        }
    }
}

#[derive(Clone, Debug)]
struct RecordedRequest {
    message: RpcRequest,
    metadata: tonic::metadata::MetadataMap,
}

enum MockResponse {
    Success,
    Failure(Status),
    Delayed(Duration, Result<(), Status>),
    Pending,
}

#[derive(Default)]
struct MockState {
    expectations: VecDeque<(RpcMethod, MockResponse)>,
    requests: Vec<RecordedRequest>,
    unexpected: Vec<String>,
}

#[derive(Default)]
struct MockShared {
    state: Mutex<MockState>,
    received: Condvar,
}

/// A strict, ordered script of responses, shared with the server thread.
#[derive(Clone, Default)]
struct MockVmService(Arc<MockShared>);

impl MockVmService {
    fn expect(&self, method: RpcMethod, response: MockResponse) {
        self.0
            .state
            .lock()
            .expectations
            .push_back((method, response));
    }

    fn requests(&self) -> Vec<RecordedRequest> {
        self.0.state.lock().requests.clone()
    }

    /// Synchronizes cancellation/concurrency tests without sleeps in the test thread.
    fn wait_for_requests(&self, count: usize, timeout: Duration) {
        let deadline = Instant::now() + timeout;
        let mut state = self.0.state.lock();
        while state.requests.len() < count {
            let timed_out = self.0.received.wait_until(&mut state, deadline).timed_out();
            assert!(
                !timed_out || state.requests.len() >= count,
                "expected at least {count} RPCs, received {}",
                state.requests.len()
            );
        }
    }

    fn assert_finished(&self) {
        let state = self.0.state.lock();
        assert!(state.unexpected.is_empty(), "{:?}", state.unexpected);
        let remaining: Vec<_> = state
            .expectations
            .iter()
            .map(|(method, _)| method)
            .collect();
        assert!(
            remaining.is_empty(),
            "unconsumed RPC expectations: {remaining:?}"
        );
    }

    fn unsupported<T>(&self, method: &str) -> Result<Response<T>, Status> {
        let error = format!("unexpected RPC {method}: not used by wslopenvmm");
        self.0.state.lock().unexpected.push(error.clone());
        Err(Status::unimplemented(error))
    }

    async fn record<T>(
        &self,
        request: Request<T>,
        message: impl FnOnce(T) -> RpcRequest,
    ) -> Result<Response<()>, Status> {
        let (metadata, _, body) = request.into_parts();
        let message = message(body);
        let method = message.method();
        let response = {
            let mut state = self.0.state.lock();
            state.requests.push(RecordedRequest { message, metadata });
            self.0.received.notify_all();
            match state.expectations.pop_front() {
                Some((expected, response)) if expected == method => response,
                expected => {
                    let expected = expected.map(|(method, _)| method);
                    let error = format!("unexpected RPC {method:?}; expected {expected:?}");
                    state.unexpected.push(error.clone());
                    return Err(Status::internal(error));
                }
            }
        };
        match response {
            MockResponse::Success => Ok(Response::new(())),
            MockResponse::Failure(status) => Err(status),
            MockResponse::Delayed(delay, result) => {
                tokio::time::sleep(delay).await;
                result.map(Response::new)
            }
            MockResponse::Pending => std::future::pending().await,
        }
    }
}

#[tonic::async_trait]
impl vmservice::vm_server::Vm for MockVmService {
    async fn create_vm(
        &self,
        request: Request<vmservice::CreateVmRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(request, |body| RpcRequest::CreateVm(Box::new(body)))
            .await
    }

    async fn resume_vm(&self, request: Request<()>) -> Result<Response<()>, Status> {
        self.record(request, |()| RpcRequest::ResumeVm).await
    }

    async fn teardown_vm(&self, request: Request<()>) -> Result<Response<()>, Status> {
        self.record(request, |()| RpcRequest::TeardownVm).await
    }

    async fn quit(&self, request: Request<()>) -> Result<Response<()>, Status> {
        self.record(request, |()| RpcRequest::Quit).await
    }

    async fn modify_resource(
        &self,
        request: Request<vmservice::ModifyResourceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(request, |body| RpcRequest::ModifyResource(Box::new(body)))
            .await
    }

    async fn add_vpci_device(
        &self,
        request: Request<vmservice::AddVpciDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(request, |body| RpcRequest::AddVpciDevice(Box::new(body)))
            .await
    }

    async fn remove_vpci_device(
        &self,
        request: Request<vmservice::RemoveVpciDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(request, RpcRequest::RemoveVpciDevice).await
    }

    async fn pause_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        self.unsupported("PauseVm")
    }

    async fn wait_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        self.unsupported("WaitVm")
    }

    async fn capabilities_vm(
        &self,
        _: Request<()>,
    ) -> Result<Response<vmservice::CapabilitiesVmResponse>, Status> {
        self.unsupported("CapabilitiesVm")
    }

    async fn properties_vm(
        &self,
        _: Request<vmservice::PropertiesVmRequest>,
    ) -> Result<Response<vmservice::PropertiesVmResponse>, Status> {
        self.unsupported("PropertiesVm")
    }

    async fn add_pcie_device(
        &self,
        _: Request<vmservice::AddPcieDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.unsupported("AddPcieDevice")
    }

    async fn remove_pcie_device(
        &self,
        _: Request<vmservice::RemovePcieDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.unsupported("RemovePcieDevice")
    }
}

struct SocketDirectory(PathBuf);

impl SocketDirectory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!("wsl-rpc-{:?}", GUID::new().unwrap()));
        std::fs::create_dir(&path).expect("create mock socket directory");
        Self(path)
    }

    fn socket_path(&self) -> PathBuf {
        self.0.join("s")
    }

    fn listen(&self) -> io::Result<Socket> {
        let listener = Socket::new(Domain::UNIX, Type::STREAM, None)?;
        listener.bind(&SockAddr::unix(self.socket_path())?)?;
        listener.listen(16)?;
        listener.set_nonblocking(true)?;
        Ok(listener)
    }
}

impl Drop for SocketDirectory {
    fn drop(&mut self) {
        match std::fs::remove_file(self.socket_path()) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => check_cleanup(Err(error)),
        }
        check_cleanup(std::fs::remove_dir(&self.0));
    }
}

/// Owns an AF_UNIX server without launching OpenVMM or changing production seams.
struct MockServerFixture {
    service: MockVmService,
    shutdown: Option<oneshot::Sender<()>>,
    thread: Option<thread::JoinHandle<io::Result<()>>>,
    directory: SocketDirectory,
}

impl MockServerFixture {
    fn new(service: MockVmService) -> Self {
        let directory = SocketDirectory::new();
        let listener = directory.listen().expect("listen on mock AF_UNIX socket");
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("create mock server runtime");
        let (shutdown, receive) = oneshot::channel();
        let server = service.clone();
        let thread = thread::spawn(move || {
            runtime.block_on(async move {
                let (send, incoming) = tokio::sync::mpsc::channel(16);
                let server = tonic::transport::Server::builder()
                    .add_service(vmservice::vm_server::VmServer::new(server))
                    .serve_with_incoming(tokio_stream::wrappers::ReceiverStream::new(incoming));
                // Drop the server/runtime rather than waiting for deliberately pending RPCs.
                tokio::select! {
                    result = server => result.map_err(io::Error::other),
                    result = accept_connections(listener, send) => result,
                    _ = receive => Ok(()),
                }
            })
        });
        Self {
            service,
            shutdown: Some(shutdown),
            thread: Some(thread),
            directory,
        }
    }

    fn socket_path(&self) -> PathBuf {
        self.directory.socket_path()
    }

    /// Also usable mid-test to simulate the OpenVMM peer disconnecting.
    fn stop(&mut self) {
        // Closing the channel stops the server even if a test is unwinding.
        drop(self.shutdown.take());
        if let Some(thread) = self.thread.take() {
            match thread.join() {
                Ok(result) => check_cleanup(result),
                Err(error) => check_cleanup(Err(error)),
            }
        }
    }
}

impl Drop for MockServerFixture {
    fn drop(&mut self) {
        self.stop();
        if !thread::panicking() {
            self.service.assert_finished();
        }
    }
}

async fn accept_connections(
    listener: Socket,
    send: tokio::sync::mpsc::Sender<io::Result<tokio::net::TcpStream>>,
) -> io::Result<()> {
    loop {
        let socket = match listener.accept() {
            Ok((socket, _)) => socket,
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                tokio::time::sleep(Duration::from_millis(2)).await;
                continue;
            }
            Err(error) => return Err(error),
        };
        socket.set_nonblocking(true)?;
        let stream = tokio::net::TcpStream::from_std(socket.into())?;
        send.send(Ok(stream)).await.map_err(|_| {
            io::Error::new(io::ErrorKind::BrokenPipe, "mock server stopped accepting")
        })?;
    }
}

fn check_cleanup(result: Result<(), impl std::fmt::Debug>) {
    if let Err(error) = result {
        if thread::panicking() {
            eprintln!("mock fixture cleanup failed: {error:?}");
        } else {
            panic!("mock fixture cleanup failed: {error:?}");
        }
    }
}

/// Owns the ABI config pointer, including its consumption by successful creation.
struct ConfigFixture {
    handle: *mut crate::WslOpenVmmConfig,
}

impl ConfigFixture {
    fn new() -> Self {
        let mut fixture = Self {
            handle: ptr::null_mut(),
        };
        // SAFETY: The out pointer refers to a live, exclusively borrowed field.
        let result = unsafe { crate::WslOpenVmmCreateConfig(&mut fixture.handle) };
        assert_eq!(result, S_OK.0);
        assert!(!fixture.handle.is_null());
        fixture
    }

    /// Borrowed handle; callers must not destroy it or transfer its ownership.
    fn as_ptr(&self) -> *mut crate::WslOpenVmmConfig {
        self.handle
    }

    fn create_vm(
        &mut self,
        socket_path: &std::path::Path,
        timeout_ms: u32,
    ) -> Result<VmFixture, HRESULT> {
        use std::os::windows::ffi::OsStrExt;

        let path: Vec<u16> = socket_path
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect();
        let mut vm = VmFixture {
            handle: ptr::null_mut(),
        };
        // SAFETY: Both out pointers are live and exclusive, the config is owned here,
        // and the null-terminated path remains valid for the entire call.
        let result = HRESULT(unsafe {
            crate::WslOpenVmmCreateVm(&mut self.handle, path.as_ptr(), timeout_ms, &mut vm.handle)
        });
        if result.is_err() {
            assert!(vm.handle.is_null(), "failed creation returned a VM handle");
            return Err(result);
        }
        assert!(
            self.handle.is_null(),
            "successful creation did not consume the config"
        );
        assert!(
            !vm.handle.is_null(),
            "successful creation returned a null VM handle"
        );
        Ok(vm)
    }
}

impl Drop for ConfigFixture {
    fn drop(&mut self) {
        // SAFETY: This fixture exclusively owns the handle and its storage;
        // a consumed/null handle is also accepted.
        unsafe { crate::WslOpenVmmDestroyConfig(&mut self.handle) };
    }
}

struct VmFixture {
    handle: *mut crate::WslOpenVmmVm,
}

impl VmFixture {
    /// Borrowed handle; callers must not destroy it or transfer its ownership.
    fn as_ptr(&self) -> *mut crate::WslOpenVmmVm {
        self.handle
    }

    fn client(&self) -> &crate::client::VmHandle {
        // SAFETY: A returned VmFixture owns a non-null VM for the lifetime of this borrow.
        unsafe { &(*self.handle).0 }
    }
}

impl Drop for VmFixture {
    fn drop(&mut self) {
        // SAFETY: This fixture exclusively owns the handle and its storage;
        // destruction also accepts a null handle.
        unsafe { crate::WslOpenVmmDestroyVm(&mut self.handle) };
    }
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

mod config_lifetime {
    use super::*;

    #[test]
    fn create_config_returns_non_null_handle() {
        let config = ConfigFixture::new();
        assert!(!config.as_ptr().is_null());
    }

    #[test]
    fn destroy_config_nullifies_handle() {
        let mut config = ConfigFixture::new();
        let mut handle = std::mem::replace(&mut config.handle, ptr::null_mut());
        // SAFETY: Ownership was transferred out of the fixture so a failed assertion
        // cannot cause it to destroy the same handle again.
        unsafe { crate::WslOpenVmmDestroyConfig(&mut handle) };
        assert!(handle.is_null());
    }

    #[test]
    fn create_vm_consumes_config_handle() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        let server = MockServerFixture::new(service);
        let mut config = ConfigFixture::new();

        let vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM using mock service");

        assert!(config.as_ptr().is_null());
        assert!(!vm.as_ptr().is_null());
    }
}

mod vm_lifetime {
    use super::*;

    #[test]
    fn create_vm_returns_non_null_handle() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        let server = MockServerFixture::new(service);
        let mut config = ConfigFixture::new();

        let vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM using mock service");

        assert!(!vm.as_ptr().is_null());
    }

    #[test]
    fn destroy_vm_nullifies_handle() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        let server = MockServerFixture::new(service);
        let mut config = ConfigFixture::new();

        let mut vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM using mock service");
        let mut handle = std::mem::replace(&mut vm.handle, ptr::null_mut());
        // SAFETY: Ownership was transferred out of the fixture so a failed assertion
        // cannot cause it to destroy the same handle again.
        unsafe { crate::WslOpenVmmDestroyVm(&mut handle) };
        assert!(handle.is_null());
    }
}

mod rpc_path {
    use super::*;
    use crate::rpc;
    use windows::Win32::Foundation::E_ABORT;

    const TIMEOUT_MS: u32 = 1_000;
    const MAX_WAIT: Duration = Duration::from_secs(5);

    fn configured_config() -> ConfigFixture {
        let config = ConfigFixture::new();
        let kernel = wide(r"C:\kernels\bzImage");
        // SAFETY: The fixture owns the config and the null-terminated kernel path
        // remains live throughout these calls.
        unsafe {
            assert_eq!(
                crate::WslOpenVmmConfigSetKernelPath(config.as_ptr(), kernel.as_ptr()),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigSetMemoryMb(config.as_ptr(), 4096),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigSetProcessorCount(config.as_ptr(), 4),
                S_OK.0
            );
        }
        config
    }

    fn assert_created_config(service: &MockVmService) {
        let requests = service.requests();
        let Some(RecordedRequest {
            message: RpcRequest::CreateVm(request),
            ..
        }) = requests.first()
        else {
            panic!("expected a dispatched CreateVm request");
        };
        assert_eq!(
            request.config,
            Some(vmservice::VmConfig {
                boot_config: Some(vmservice::vm_config::BootConfig::DirectBoot(
                    vmservice::DirectBoot {
                        kernel_path: r"C:\kernels\bzImage".to_string(),
                        ..Default::default()
                    },
                )),
                memory_config: Some(vmservice::MemoryConfig {
                    memory_mb: 4096,
                    ..Default::default()
                }),
                processor_config: Some(vmservice::ProcessorConfig {
                    processor_count: 4,
                    ..Default::default()
                }),
                ..Default::default()
            })
        );
    }

    fn assert_rpc_methods(service: &MockVmService, expected: &[RpcMethod]) {
        let methods: Vec<_> = service
            .requests()
            .iter()
            .map(|request| request.message.method())
            .collect();
        assert_eq!(methods, expected);
    }

    fn assert_timeout_elapsed(elapsed: Duration) {
        // Allow timer/transport rounding, but reject an immediate synthetic timeout.
        assert!(
            elapsed >= Duration::from_millis(950),
            "RPC timed out early: {elapsed:?}"
        );
        assert!(
            elapsed < MAX_WAIT,
            "RPC exceeded its deadline allowance: {elapsed:?}"
        );
    }

    fn cleanup_vm(vm: &VmFixture) {
        // SAFETY: The fixture owns a live VM throughout these non-consuming calls.
        unsafe {
            assert_eq!(crate::WslOpenVmmVmTeardown(vm.as_ptr()), S_OK.0);
            assert_eq!(crate::WslOpenVmmVmQuit(vm.as_ptr()), S_OK.0);
        }
    }

    #[test]
    fn config_builder_creates_usable_vm_over_rpc() {
        let service = MockVmService::default();
        for method in [
            RpcMethod::CreateVm,
            RpcMethod::ResumeVm,
            RpcMethod::TeardownVm,
            RpcMethod::Quit,
        ] {
            service.expect(method, MockResponse::Success);
        }
        let server = MockServerFixture::new(service.clone());
        let mut config = configured_config();
        let vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM over RPC");

        assert!(config.as_ptr().is_null());
        assert!(!vm.as_ptr().is_null());
        assert_created_config(&service);
        // SAFETY: The fixture owns a live VM throughout the call.
        assert_eq!(unsafe { crate::WslOpenVmmVmResume(vm.as_ptr()) }, S_OK.0);
        cleanup_vm(&vm);
        assert_rpc_methods(
            &service,
            &[
                RpcMethod::CreateVm,
                RpcMethod::ResumeVm,
                RpcMethod::TeardownVm,
                RpcMethod::Quit,
            ],
        );
    }

    #[test]
    fn dynamic_consomme_operations_preserve_nic_and_port_configuration() {
        use vmservice::{
            ConsommeBackend, IpProtocol, ModifyResourceRequest, ModifyType, NicConfig, PortConfig,
            modify_resource_request::Resource, nic_config::Backend,
        };

        let service = MockVmService::default();
        for method in [
            RpcMethod::CreateVm,
            RpcMethod::ModifyResource,
            RpcMethod::ModifyResource,
            RpcMethod::ModifyResource,
            RpcMethod::TeardownVm,
            RpcMethod::Quit,
        ] {
            service.expect(method, MockResponse::Success);
        }
        let server = MockServerFixture::new(service.clone());
        let mut config = configured_config();
        let vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM over RPC");
        let nic_id = wide("11111111-1111-1111-1111-111111111111");
        let mac_address = wide("00-15-5D-01-02-03");
        let cidr = wide("192.168.1.0/24");
        let host_address = wide("fe80::1%7");

        // SAFETY: The fixture owns a live VM and all strings are null-terminated
        // and remain valid throughout the calls.
        unsafe {
            assert_eq!(
                crate::WslOpenVmmVmAddConsommeNic(
                    vm.as_ptr(),
                    nic_id.as_ptr(),
                    mac_address.as_ptr(),
                    cidr.as_ptr(),
                ),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmVmBindPort(
                    vm.as_ptr(),
                    nic_id.as_ptr(),
                    8080,
                    80,
                    1,
                    host_address.as_ptr(),
                ),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmVmUnbindPort(
                    vm.as_ptr(),
                    nic_id.as_ptr(),
                    8080,
                    80,
                    1,
                    host_address.as_ptr(),
                ),
                S_OK.0
            );
        }
        cleanup_vm(&vm);

        let requests = service.requests();
        let modify_requests: Vec<_> = requests
            .iter()
            .filter_map(|request| match &request.message {
                RpcRequest::ModifyResource(request) => Some(request.as_ref()),
                _ => None,
            })
            .collect();
        let nic_config = |ports| NicConfig {
            nic_id: "11111111-1111-1111-1111-111111111111".to_string(),
            mac_address: String::new(),
            backend: Some(Backend::Consomme(ConsommeBackend {
                cidr: String::new(),
                ports,
            })),
            ..Default::default()
        };
        assert_eq!(
            modify_requests,
            [
                &ModifyResourceRequest {
                    r#type: ModifyType::Add as i32,
                    resource: Some(Resource::NicConfig(NicConfig {
                        nic_id: "11111111-1111-1111-1111-111111111111".to_string(),
                        mac_address: "00-15-5D-01-02-03".to_string(),
                        backend: Some(Backend::Consomme(ConsommeBackend {
                            cidr: "192.168.1.0/24".to_string(),
                            ports: Vec::new(),
                        })),
                        ..Default::default()
                    })),
                },
                &ModifyResourceRequest {
                    r#type: ModifyType::Update as i32,
                    resource: Some(Resource::NicConfig(nic_config(vec![PortConfig {
                        host_port: 8080,
                        guest_port: 80,
                        protocol: IpProtocol::Tcp as i32,
                        host_address: "fe80::1%7".to_string(),
                    }]))),
                },
                &ModifyResourceRequest {
                    r#type: ModifyType::Remove as i32,
                    resource: Some(Resource::NicConfig(nic_config(vec![PortConfig {
                        host_port: 8080,
                        guest_port: 80,
                        protocol: IpProtocol::Tcp as i32,
                        host_address: "fe80::1%7".to_string(),
                    }]))),
                },
            ]
        );
    }

    #[test]
    fn create_vm_timeout_preserves_config_and_rejects_retry() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Pending);
        let server = MockServerFixture::new(service.clone());
        let mut config = configured_config();
        let original_handle = config.as_ptr();

        let started = Instant::now();
        let result = config.create_vm(&server.socket_path(), TIMEOUT_MS);
        let elapsed = started.elapsed();
        assert_eq!(result.err(), Some(rpc::TIMEOUT));
        assert_timeout_elapsed(elapsed);
        assert_eq!(config.as_ptr(), original_handle);
        assert_created_config(&service);

        assert_eq!(
            config.create_vm(&server.socket_path(), TIMEOUT_MS).err(),
            Some(rpc::INVALID_STATE)
        );
        assert_eq!(config.as_ptr(), original_handle);
        assert_rpc_methods(&service, &[RpcMethod::CreateVm]);
    }

    #[test]
    fn cancel_requests_aborts_in_flight_rpc_and_allows_cleanup() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        service.expect(RpcMethod::ResumeVm, MockResponse::Pending);
        service.expect(RpcMethod::TeardownVm, MockResponse::Success);
        service.expect(RpcMethod::Quit, MockResponse::Success);
        let server = MockServerFixture::new(service.clone());
        let mut config = configured_config();
        let vm = config
            .create_vm(&server.socket_path(), 10_000)
            .expect("create VM over RPC");
        let client = vm.client();

        thread::scope(|scope| {
            let operation = scope.spawn(|| client.resume_vm());
            service.wait_for_requests(2, MAX_WAIT);
            let started = Instant::now();
            // SAFETY: The fixture owns the VM until the scoped operation has joined.
            // Cancellation is permitted to race an operation, but not destruction.
            assert_eq!(
                unsafe { crate::WslOpenVmmVmCancelRequests(vm.as_ptr()) },
                S_OK.0
            );
            assert_eq!(operation.join().expect("join pending RPC"), E_ABORT);
            assert!(
                started.elapsed() < MAX_WAIT,
                "cancellation waited for the RPC deadline"
            );
        });

        // SAFETY: The fixture still owns the VM; invalid state must be rejected locally.
        assert_eq!(
            unsafe { crate::WslOpenVmmVmResume(vm.as_ptr()) },
            rpc::INVALID_STATE.0
        );
        cleanup_vm(&vm);
        assert_rpc_methods(
            &service,
            &[
                RpcMethod::CreateVm,
                RpcMethod::ResumeVm,
                RpcMethod::TeardownVm,
                RpcMethod::Quit,
            ],
        );
    }

    #[test]
    fn operation_timeout_requires_recovery_and_allows_cleanup() {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        service.expect(RpcMethod::ResumeVm, MockResponse::Pending);
        service.expect(RpcMethod::TeardownVm, MockResponse::Success);
        service.expect(RpcMethod::Quit, MockResponse::Success);
        let server = MockServerFixture::new(service.clone());
        let mut config = configured_config();
        let vm = config
            .create_vm(&server.socket_path(), TIMEOUT_MS)
            .expect("create VM over RPC");

        let started = Instant::now();
        // SAFETY: The fixture owns the VM throughout the operation.
        let result = unsafe { crate::WslOpenVmmVmResume(vm.as_ptr()) };
        let elapsed = started.elapsed();
        assert_eq!(result, rpc::TIMEOUT.0);
        assert_timeout_elapsed(elapsed);
        assert_rpc_methods(&service, &[RpcMethod::CreateVm, RpcMethod::ResumeVm]);

        // SAFETY: The fixture still owns the VM; invalid state must be rejected locally.
        assert_eq!(
            unsafe { crate::WslOpenVmmVmResume(vm.as_ptr()) },
            rpc::INVALID_STATE.0
        );
        cleanup_vm(&vm);
        assert_rpc_methods(
            &service,
            &[
                RpcMethod::CreateVm,
                RpcMethod::ResumeVm,
                RpcMethod::TeardownVm,
                RpcMethod::Quit,
            ],
        );
    }
}

mod config_builder {
    use super::*;
    use vmservice::{
        ConsommeBackend, DevicesConfig, DirectBoot, DiskType, HvSocketConfig, MemoryConfig,
        NicConfig, ProcessorConfig, ScsiDisk, SerialConfig, VirtioConsoleConfig, VmConfig,
        nic_config::Backend, vm_config::BootConfig,
    };

    fn assert_built_config(mut config: ConfigFixture, expected: VmConfig) {
        let service = MockVmService::default();
        service.expect(RpcMethod::CreateVm, MockResponse::Success);
        let server = MockServerFixture::new(service.clone());
        let _vm = config
            .create_vm(&server.socket_path(), 5_000)
            .expect("create VM using mock service");

        let requests = service.requests();
        assert_eq!(requests.len(), 1);
        let RpcRequest::CreateVm(request) = &requests[0].message else {
            panic!("expected CreateVm request");
        };
        assert_eq!(request.config, Some(expected));
    }

    fn set_string(
        config: &ConfigFixture,
        setter: unsafe extern "C" fn(*mut crate::WslOpenVmmConfig, *const u16) -> i32,
        value: &str,
    ) {
        let value = wide(value);
        // SAFETY: The fixture owns a live config and the null-terminated string
        // remains valid throughout the setter call.
        assert_eq!(unsafe { setter(config.as_ptr(), value.as_ptr()) }, S_OK.0);
    }

    #[test]
    fn new_config_has_default_parameters() {
        assert_built_config(ConfigFixture::new(), VmConfig::default());
    }

    #[test]
    fn set_kernel_path_sets_direct_boot_and_replaces_previous_value() {
        let config = ConfigFixture::new();
        let path = "C:\\kernels\\\u{6d4b}\u{8bd5}\\\u{1f427}\\bzImage";
        for value in [r"C:\old-kernel", path] {
            set_string(&config, crate::WslOpenVmmConfigSetKernelPath, value);
        }
        assert_built_config(
            config,
            VmConfig {
                boot_config: Some(BootConfig::DirectBoot(DirectBoot {
                    kernel_path: path.to_string(),
                    ..Default::default()
                })),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_initrd_path_sets_direct_boot_and_replaces_previous_value() {
        let config = ConfigFixture::new();
        for value in [r"C:\old-initrd", r"C:\boot files\initrd.img"] {
            set_string(&config, crate::WslOpenVmmConfigSetInitrdPath, value);
        }
        assert_built_config(
            config,
            VmConfig {
                boot_config: Some(BootConfig::DirectBoot(DirectBoot {
                    initrd_path: r"C:\boot files\initrd.img".to_string(),
                    ..Default::default()
                })),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_kernel_cmdline_sets_direct_boot_and_replaces_previous_value() {
        let config = ConfigFixture::new();
        for value in ["old-command-line", "console=hvc0 root=/dev/sda rw quiet"] {
            set_string(&config, crate::WslOpenVmmConfigSetKernelCmdLine, value);
        }
        assert_built_config(
            config,
            VmConfig {
                boot_config: Some(BootConfig::DirectBoot(DirectBoot {
                    kernel_cmdline: "console=hvc0 root=/dev/sda rw quiet".to_string(),
                    ..Default::default()
                })),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_memory_mb_preserves_u64_and_replaces_previous_value() {
        let config = ConfigFixture::new();
        let memory_mb = u64::from(u32::MAX) + 1024;
        for value in [2048, memory_mb] {
            // SAFETY: The fixture owns a live config for the duration of the call.
            assert_eq!(
                unsafe { crate::WslOpenVmmConfigSetMemoryMb(config.as_ptr(), value) },
                S_OK.0
            );
        }
        assert_built_config(
            config,
            VmConfig {
                memory_config: Some(MemoryConfig {
                    memory_mb,
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_processor_count_replaces_previous_value() {
        let config = ConfigFixture::new();
        for value in [2, 8] {
            // SAFETY: The fixture owns a live config for the duration of the call.
            assert_eq!(
                unsafe { crate::WslOpenVmmConfigSetProcessorCount(config.as_ptr(), value) },
                S_OK.0
            );
        }
        assert_built_config(
            config,
            VmConfig {
                processor_config: Some(ProcessorConfig {
                    processor_count: 8,
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_hvsocket_path_replaces_previous_value() {
        let config = ConfigFixture::new();
        for value in [r"C:\old-hvsocket", r"C:\sockets\hvsocket"] {
            set_string(&config, crate::WslOpenVmmConfigSetHvSocketPath, value);
        }
        assert_built_config(
            config,
            VmConfig {
                hvsocket_config: Some(HvSocketConfig {
                    path: r"C:\sockets\hvsocket".to_string(),
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn add_boot_disk_appends_disks_with_expected_fields_and_types() {
        let config = ConfigFixture::new();
        let mut disks = Vec::new();
        for (controller, lun, path, read_only, expected_type, expected_read_only) in [
            (
                2,
                7,
                r"C:\disks\legacy.vhd",
                0,
                DiskType::ScsiDiskTypeVhd1,
                false,
            ),
            (
                3,
                5,
                r"C:\disks\root.vhdx",
                1,
                DiskType::ScsiDiskTypeVhdx,
                true,
            ),
            (
                1,
                9,
                r"C:\disks\data.VHDX",
                -1,
                DiskType::ScsiDiskTypeVhdx,
                true,
            ),
            (
                4,
                2,
                r"C:\disks\archive.vhdx\",
                0,
                DiskType::ScsiDiskTypeVhdx,
                false,
            ),
        ] {
            let host_path = wide(path);
            // SAFETY: The fixture owns a live config and host_path is null-terminated
            // and remains valid throughout the call.
            assert_eq!(
                unsafe {
                    crate::WslOpenVmmConfigAddBootDisk(
                        config.as_ptr(),
                        controller,
                        lun,
                        host_path.as_ptr(),
                        read_only,
                    )
                },
                S_OK.0
            );
            disks.push(ScsiDisk {
                controller,
                lun,
                host_path: path.to_string(),
                r#type: expected_type as i32,
                read_only: expected_read_only,
            });
        }
        assert_built_config(
            config,
            VmConfig {
                devices_config: Some(DevicesConfig {
                    scsi_disks: disks,
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_consomme_nic_appends_nics_with_default_backend() {
        let config = ConfigFixture::new();
        let mut nics = Vec::new();
        for (id, mac) in [
            ("11111111-1111-1111-1111-111111111111", "00-15-5D-01-02-03"),
            ("22222222-2222-2222-2222-222222222222", "00-15-5D-04-05-06"),
        ] {
            let nic_id = wide(id);
            let mac_address = wide(mac);
            // SAFETY: The config is live and both strings are null-terminated
            // and remain valid throughout the call.
            assert_eq!(
                unsafe {
                    crate::WslOpenVmmConfigSetConsommeNic(
                        config.as_ptr(),
                        nic_id.as_ptr(),
                        mac_address.as_ptr(),
                    )
                },
                S_OK.0
            );
            nics.push(NicConfig {
                nic_id: id.to_string(),
                mac_address: mac.to_string(),
                backend: Some(Backend::Consomme(ConsommeBackend::default())),
                ..Default::default()
            });
        }
        assert_built_config(
            config,
            VmConfig {
                devices_config: Some(DevicesConfig {
                    nic_config: nics,
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn add_serial_port_appends_ports_in_connect_mode() {
        let config = ConfigFixture::new();
        let mut ports = Vec::new();
        for (port, path) in [(0, r"\\.\pipe\serial0"), (2, r"\\.\pipe\serial2")] {
            let pipe_name = wide(path);
            // SAFETY: The config is live and pipe_name is null-terminated
            // and remains valid throughout the call.
            assert_eq!(
                unsafe {
                    crate::WslOpenVmmConfigAddSerialPort(config.as_ptr(), port, pipe_name.as_ptr())
                },
                S_OK.0
            );
            ports.push(vmservice::serial_config::Config {
                port,
                socket_path: path.to_string(),
                connect: true,
            });
        }
        assert_built_config(
            config,
            VmConfig {
                serial_config: Some(SerialConfig { ports }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn set_virtio_console_path_replaces_previous_value_in_connect_mode() {
        let config = ConfigFixture::new();
        for value in [r"\\.\pipe\old-console", r"\\.\pipe\virtio-console"] {
            set_string(&config, crate::WslOpenVmmConfigSetVirtioConsolePath, value);
        }
        assert_built_config(
            config,
            VmConfig {
                devices_config: Some(DevicesConfig {
                    virtio_console: Some(VirtioConsoleConfig {
                        socket_path: r"\\.\pipe\virtio-console".to_string(),
                        connect: true,
                    }),
                    ..Default::default()
                }),
                ..Default::default()
            },
        );
    }

    #[test]
    fn setters_preserve_other_config_parameters() {
        let config = ConfigFixture::new();
        set_string(
            &config,
            crate::WslOpenVmmConfigSetKernelPath,
            r"C:\old-kernel",
        );
        set_string(
            &config,
            crate::WslOpenVmmConfigSetInitrdPath,
            r"C:\initrd.img",
        );
        set_string(
            &config,
            crate::WslOpenVmmConfigSetKernelCmdLine,
            "console=hvc0",
        );
        set_string(
            &config,
            crate::WslOpenVmmConfigSetHvSocketPath,
            r"C:\hvsocket",
        );
        set_string(
            &config,
            crate::WslOpenVmmConfigSetVirtioConsolePath,
            r"\\.\pipe\console",
        );
        let disk = wide(r"C:\root.vhdx");
        let nic_id = wide("11111111-1111-1111-1111-111111111111");
        let mac = wide("00-15-5D-01-02-03");
        let serial = wide(r"\\.\pipe\serial0");
        // SAFETY: The config and all null-terminated strings remain live throughout these calls.
        unsafe {
            assert_eq!(
                crate::WslOpenVmmConfigSetMemoryMb(config.as_ptr(), 4096),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigSetProcessorCount(config.as_ptr(), 4),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigAddBootDisk(config.as_ptr(), 1, 2, disk.as_ptr(), 0),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigSetConsommeNic(
                    config.as_ptr(),
                    nic_id.as_ptr(),
                    mac.as_ptr()
                ),
                S_OK.0
            );
            assert_eq!(
                crate::WslOpenVmmConfigAddSerialPort(config.as_ptr(), 0, serial.as_ptr()),
                S_OK.0
            );
        }
        set_string(&config, crate::WslOpenVmmConfigSetKernelPath, r"C:\bzImage");
        set_string(
            &config,
            crate::WslOpenVmmConfigSetVirtioConsolePath,
            r"\\.\pipe\final-console",
        );

        assert_built_config(
            config,
            VmConfig {
                boot_config: Some(BootConfig::DirectBoot(DirectBoot {
                    kernel_path: r"C:\bzImage".to_string(),
                    initrd_path: r"C:\initrd.img".to_string(),
                    kernel_cmdline: "console=hvc0".to_string(),
                })),
                memory_config: Some(MemoryConfig {
                    memory_mb: 4096,
                    ..Default::default()
                }),
                processor_config: Some(ProcessorConfig {
                    processor_count: 4,
                    ..Default::default()
                }),
                hvsocket_config: Some(HvSocketConfig {
                    path: r"C:\hvsocket".to_string(),
                }),
                devices_config: Some(DevicesConfig {
                    scsi_disks: vec![ScsiDisk {
                        controller: 1,
                        lun: 2,
                        host_path: r"C:\root.vhdx".to_string(),
                        r#type: DiskType::ScsiDiskTypeVhdx as i32,
                        read_only: false,
                    }],
                    nic_config: vec![NicConfig {
                        nic_id: "11111111-1111-1111-1111-111111111111".to_string(),
                        mac_address: "00-15-5D-01-02-03".to_string(),
                        backend: Some(Backend::Consomme(ConsommeBackend::default())),
                        ..Default::default()
                    }],
                    virtio_console: Some(VirtioConsoleConfig {
                        socket_path: r"\\.\pipe\final-console".to_string(),
                        connect: true,
                    }),
                    ..Default::default()
                }),
                serial_config: Some(SerialConfig {
                    ports: vec![vmservice::serial_config::Config {
                        port: 0,
                        socket_path: r"\\.\pipe\serial0".to_string(),
                        connect: true,
                    }],
                }),
                ..Default::default()
            },
        );
    }
}
