// Copyright (C) Microsoft Corporation. All rights reserved.

use super::*;
use parking_lot::Mutex;
use std::sync::Arc;
use tonic::{Response, Status};
use windows::Win32::Foundation::{
    E_ABORT, E_ACCESSDENIED, E_NOTIMPL, ERROR_CONNECTION_ABORTED, ERROR_INVALID_DATA,
    ERROR_LOGON_FAILURE, ERROR_NOT_ENOUGH_MEMORY, ERROR_RETRY,
};

#[derive(Debug)]
enum Event {
    Create,
    Resume,
    Teardown,
    Quit,
    Port(ModifyResourceRequest),
    Add(vmservice::AddVpciDeviceRequest),
    Remove(vmservice::RemoveVpciDeviceRequest),
}

#[derive(Default)]
struct State {
    events: Vec<Event>,
    fail_next: bool,
    next_status: Option<tonic::Code>,
    hang_next: bool,
    observed: Option<std::sync::mpsc::Sender<()>>,
}

#[derive(Clone)]
struct Server(Arc<Mutex<State>>);

impl Server {
    async fn record(&self, event: Event) -> Result<Response<()>, Status> {
        let (status, hang) = {
            let mut state = self.0.lock();
            state.events.push(event);
            if let Some(observed) = state.observed.take() {
                observed.send(()).unwrap();
            }
            let status = if std::mem::take(&mut state.fail_next) {
                Some(tonic::Code::InvalidArgument)
            } else {
                state.next_status.take()
            };
            (status, std::mem::take(&mut state.hang_next))
        };
        if hang {
            std::future::pending::<()>().await;
        }
        if let Some(status) = status {
            Err(Status::new(status, "injected failure"))
        } else {
            Ok(Response::new(()))
        }
    }
}

#[tonic::async_trait]
impl vmservice::vm_server::Vm for Server {
    async fn create_vm(&self, _: Request<CreateVmRequest>) -> Result<Response<()>, Status> {
        self.record(Event::Create).await
    }
    async fn teardown_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        self.record(Event::Teardown).await
    }
    async fn pause_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn resume_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        self.record(Event::Resume).await
    }
    async fn wait_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn capabilities_vm(
        &self,
        _: Request<()>,
    ) -> Result<Response<vmservice::CapabilitiesVmResponse>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn properties_vm(
        &self,
        _: Request<vmservice::PropertiesVmRequest>,
    ) -> Result<Response<vmservice::PropertiesVmResponse>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn modify_resource(
        &self,
        request: Request<ModifyResourceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(Event::Port(request.into_inner())).await
    }
    async fn add_pcie_device(
        &self,
        _: Request<vmservice::AddPcieDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn remove_pcie_device(
        &self,
        _: Request<vmservice::RemovePcieDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn add_vpci_device(
        &self,
        request: Request<vmservice::AddVpciDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(Event::Add(request.into_inner())).await
    }
    async fn remove_vpci_device(
        &self,
        request: Request<vmservice::RemoveVpciDeviceRequest>,
    ) -> Result<Response<()>, Status> {
        self.record(Event::Remove(request.into_inner())).await
    }
    async fn quit(&self, _: Request<()>) -> Result<Response<()>, Status> {
        self.record(Event::Quit).await
    }
}

struct Fixture {
    vm: Option<crate::WslOpenVmmVm>,
    state: Arc<Mutex<State>>,
    shutdown: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
    directory: SocketDirectory,
}

struct SocketDirectory(std::path::PathBuf);

impl SocketDirectory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!("wsl-rpc-{:?}", GUID::new().unwrap()));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }

    fn socket_path(&self) -> std::path::PathBuf {
        self.0.join("s")
    }

    fn listen(&self) -> socket2::Socket {
        let listener =
            socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::STREAM, None).unwrap();
        listener
            .bind(&socket2::SockAddr::unix(self.socket_path()).unwrap())
            .unwrap();
        listener.listen(16).unwrap();
        listener.set_nonblocking(true).unwrap();
        listener
    }
}

impl Drop for SocketDirectory {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

impl Fixture {
    fn new() -> Self {
        Self::with_timeout(Duration::from_secs(5))
    }

    fn with_timeout(timeout: Duration) -> Self {
        let directory = SocketDirectory::new();
        let listener = directory.listen();
        let state = Arc::new(Mutex::new(State::default()));
        let server = Server(state.clone());
        let (shutdown, receive) = tokio::sync::oneshot::channel();
        let thread = std::thread::spawn(move || {
            tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .unwrap()
                .block_on(async {
                    let (send, incoming) = tokio::sync::mpsc::channel(16);
                    tokio::spawn(async move {
                        loop {
                            let stream = match listener.accept() {
                                Ok((socket, _)) => {
                                    socket.set_nonblocking(true).unwrap();
                                    tokio::net::TcpStream::from_std(socket.into())
                                }
                                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                                    tokio::time::sleep(Duration::from_millis(2)).await;
                                    continue;
                                }
                                Err(error) => Err(error),
                            };
                            if send.send(stream).await.is_err() {
                                break;
                            }
                        }
                    });
                    tonic::transport::Server::builder()
                        .add_service(vmservice::vm_server::VmServer::new(server))
                        .serve_with_incoming_shutdown(
                            tokio_stream::wrappers::ReceiverStream::new(incoming),
                            async {
                                let _ = receive.await;
                            },
                        )
                        .await
                        .unwrap();
                });
        });
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let client = VmClient::new(
            runtime
                .block_on(af_unix::connect_channel(
                    directory.socket_path(),
                    Duration::from_secs(5),
                ))
                .unwrap(),
        );
        Self {
            vm: Some(crate::WslOpenVmmVm::new(VmHandle::new(
                runtime, client, timeout,
            ))),
            state,
            shutdown: Some(shutdown),
            thread: Some(thread),
            directory,
        }
    }

    fn handle(&mut self) -> *mut crate::WslOpenVmmVm {
        self.vm.as_mut().unwrap()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        drop(self.vm.take());
        let _ = self.shutdown.take().unwrap().send(());
        self.thread.take().unwrap().join().unwrap();
    }
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(Some(0)).collect()
}

#[test]
fn port_abi_preserves_operation_protocol_and_family() {
    let mut fixture = Fixture::new();
    for (family, address) in [
        (i32::from(AF_INET), "127.0.0.1"),
        (i32::from(AF_INET6), "::1"),
    ] {
        for tcp in [0, 1] {
            unsafe {
                assert_eq!(
                    crate::WslOpenVmmVmBindPort(fixture.handle(), 8080, 80, tcp, family),
                    S_OK.0
                );
                assert_eq!(
                    crate::WslOpenVmmVmUnbindPort(fixture.handle(), 8080, 80, tcp, family),
                    S_OK.0
                );
            }
            let events = std::mem::take(&mut fixture.state.lock().events);
            assert_eq!(events.len(), 2);
            for (event, operation) in events
                .into_iter()
                .zip([ModifyType::Update, ModifyType::Remove])
            {
                let Event::Port(request) = event else {
                    panic!("expected port request")
                };
                assert_eq!(request.r#type, operation as i32);
                let Some(vmservice::modify_resource_request::Resource::NicConfig(nic)) =
                    request.resource
                else {
                    panic!("expected NIC")
                };
                let Some(vmservice::nic_config::Backend::Consomme(backend)) = nic.backend else {
                    panic!("expected Consomme")
                };
                assert_eq!(
                    backend.ports,
                    vec![PortConfig {
                        host_port: 8080,
                        guest_port: 80,
                        protocol: if tcp != 0 {
                            vmservice::IpProtocol::Tcp
                        } else {
                            vmservice::IpProtocol::Udp
                        } as i32,
                        host_address: address.to_string(),
                    }]
                );
            }
        }
    }
    unsafe {
        assert_eq!(
            crate::WslOpenVmmVmBindPort(fixture.handle(), 8080, 80, 1, 0),
            E_INVALIDARG.0
        );
        assert_eq!(
            crate::WslOpenVmmVmUnbindPort(fixture.handle(), 8080, 80, 1, 0),
            E_INVALIDARG.0
        );
    }
    assert!(fixture.state.lock().events.is_empty());
}

#[test]
fn share_abi_preserves_fields_and_retries() {
    let mut fixture = Fixture::new();
    let tag = wide("share");
    let path = wide(r"C:\share");
    fixture.state.lock().fail_next = true;
    unsafe {
        assert_ne!(
            crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 1),
            S_OK.0
        );
        assert_eq!(
            crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 1),
            S_OK.0
        );
        assert_eq!(
            crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0),
            HRESULT::from_win32(ERROR_ALREADY_EXISTS.0).0
        );
    }

    let events = std::mem::take(&mut fixture.state.lock().events);
    assert_eq!(events.len(), 2);
    let Event::Add(request) = &events[1] else {
        panic!("expected add")
    };
    assert_ne!(request.instance_id, String::new());
    let instance_id = request.instance_id.clone();
    let Some(vmservice::pcie_device_kind::Kind::Virtio(device)) =
        &request.device.as_ref().unwrap().kind
    else {
        panic!("expected virtio")
    };
    let Some(vmservice::virtio_device::Kind::Fs(fs)) = &device.kind else {
        panic!("expected filesystem")
    };
    assert_eq!(fs.tag, "share");
    assert_eq!(fs.root_path, r"C:\share");
    assert!(fs.read_only);
    fixture.state.lock().fail_next = true;
    unsafe {
        assert_ne!(
            crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()),
            S_OK.0
        );
        assert_eq!(
            crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()),
            S_OK.0
        );
        assert_eq!(
            crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()),
            HRESULT::from_win32(ERROR_NOT_FOUND.0).0
        );
    }
    let events = std::mem::take(&mut fixture.state.lock().events);
    assert_eq!(events.len(), 2);
    for event in events {
        let Event::Remove(request) = event else {
            panic!("expected remove")
        };
        assert_eq!(request.instance_id, instance_id);
    }
    unsafe {
        assert_eq!(
            crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0),
            S_OK.0
        );
        assert_eq!(crate::WslOpenVmmVmTeardown(fixture.handle()), S_OK.0);
        assert_eq!(
            crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()),
            HRESULT::from_win32(ERROR_NOT_FOUND.0).0
        );
    }
    let events = std::mem::take(&mut fixture.state.lock().events);
    let Event::Add(request) = &events[0] else {
        panic!("expected add")
    };
    assert_ne!(request.instance_id, instance_id);
    let Some(vmservice::pcie_device_kind::Kind::Virtio(device)) =
        &request.device.as_ref().unwrap().kind
    else {
        panic!("expected virtio")
    };
    let Some(vmservice::virtio_device::Kind::Fs(fs)) = &device.kind else {
        panic!("expected filesystem")
    };
    assert!(!fs.read_only);
}

#[test]
fn direct_client_serializes_share_requests_and_bookkeeping() {
    let fixture = Fixture::new();
    let vm = &fixture.vm.as_ref().unwrap().0;
    let barrier = std::sync::Barrier::new(2);
    let results = std::thread::scope(|scope| {
        let add = || {
            barrier.wait();
            vm.add_share("shared".to_string(), r"C:\share".to_string(), false)
        };
        let first = scope.spawn(add);
        let second = scope.spawn(add);
        [first.join().unwrap(), second.join().unwrap()]
    });
    assert_eq!(results.iter().filter(|result| **result == S_OK).count(), 1);
    assert_eq!(
        results
            .iter()
            .filter(|result| **result == HRESULT::from_win32(ERROR_ALREADY_EXISTS.0))
            .count(),
        1
    );
    assert_eq!(fixture.state.lock().events.len(), 1);
    assert_eq!(vm.inner.lock().shares.len(), 1);
    assert_eq!(vm.remove_share("shared"), S_OK);
    assert!(vm.inner.lock().shares.is_empty());
    assert_eq!(fixture.state.lock().events.len(), 2);
}

#[test]
fn direct_client_owns_cancellation_and_cleanup_policy() {
    let fixture = Fixture::new();
    let vm = &fixture.vm.as_ref().unwrap().0;
    let lock = vm.inner.lock();
    assert_eq!(lock.status, VmStatus::Active);
    assert_eq!(vm.cancel_requests(), S_OK);
    assert!(*vm.cancellation.borrow());
    // Signaling cancellation does not acquire the operation lock.
    drop(lock);
    assert_eq!(vm.resume_vm(), rpc::INVALID_STATE);
    assert_eq!(
        vm.attach_scsi_disk(0, 0, "disk.vhdx".to_string(), false),
        rpc::INVALID_STATE
    );
    assert_eq!(vm.detach_scsi_disk(0, 0), rpc::INVALID_STATE);
    assert_eq!(
        vm.bind_port(8080, 80, true, i32::from(AF_INET)),
        rpc::INVALID_STATE
    );
    assert_eq!(
        vm.unbind_port(8080, 80, true, i32::from(AF_INET)),
        rpc::INVALID_STATE
    );
    assert_eq!(
        vm.add_share("share".to_string(), r"C:\share".to_string(), false),
        rpc::INVALID_STATE
    );
    assert_eq!(vm.remove_share("share"), rpc::INVALID_STATE);
    assert!(fixture.state.lock().events.is_empty());
    assert_eq!(vm.teardown_vm(), S_OK);
    assert_eq!(vm.quit(), S_OK);
    assert_eq!(vm.inner.lock().status, VmStatus::RecoveryRequired);
    assert_eq!(fixture.state.lock().events.len(), 2);
}

#[test]
fn cancellation_racing_success_requires_recovery() {
    for cleanup in [false, true] {
        let fixture = Fixture::new();
        let vm = &fixture.vm.as_ref().unwrap().0;
        assert_eq!(
            vm.with_operation(cleanup, |inner, _, _| {
                assert_eq!(inner.status, VmStatus::Active);
                assert_eq!(vm.cancel_requests(), S_OK);
                S_OK
            }),
            S_OK
        );
        assert_eq!(vm.inner.lock().status, VmStatus::RecoveryRequired);
        assert_eq!(vm.resume_vm(), rpc::INVALID_STATE);
        assert!(fixture.state.lock().events.is_empty());
        assert_eq!(vm.teardown_vm(), S_OK);
        assert_eq!(vm.inner.lock().status, VmStatus::RecoveryRequired);
    }
}

#[test]
fn configuration_client_owns_creation_lock_deadline() {
    let fixture = Fixture::new();
    let config = VmConfigHandle::new();
    let path = fixture
        .directory
        .socket_path()
        .to_string_lossy()
        .into_owned();
    let lock = config.builder.lock();
    assert_eq!(
        config.create_vm(path.clone(), 0).err().unwrap(),
        E_INVALIDARG
    );
    let start = Instant::now();
    assert_eq!(
        config.create_vm(path.clone(), 50).err().unwrap(),
        rpc::TIMEOUT
    );
    assert!(start.elapsed() < Duration::from_secs(2));
    assert_eq!(lock.status, VmConfigStatus::Ready);
    assert!(fixture.state.lock().events.is_empty());
    drop(lock);
    assert!(config.create_vm(path, 2000).is_ok());
    assert_eq!(fixture.state.lock().events.len(), 1);
}

#[test]
fn uncertain_share_mutation_is_not_replayed_and_cleanup_does_not_revalidate() {
    for (code, expected) in [
        (tonic::Code::Cancelled, E_ABORT),
        (tonic::Code::Unknown, E_FAIL),
        (tonic::Code::DeadlineExceeded, rpc::TIMEOUT),
        (
            tonic::Code::ResourceExhausted,
            HRESULT::from_win32(ERROR_NOT_ENOUGH_MEMORY.0),
        ),
        (tonic::Code::Aborted, HRESULT::from_win32(ERROR_RETRY.0)),
        (tonic::Code::Internal, E_FAIL),
        (
            tonic::Code::Unavailable,
            HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0),
        ),
        (
            tonic::Code::DataLoss,
            HRESULT::from_win32(ERROR_INVALID_DATA.0),
        ),
    ] {
        let mut fixture = Fixture::new();
        fixture.state.lock().next_status = Some(code);
        let tag = wide("uncertain");
        let path = wide(r"C:\share");
        unsafe {
            assert_eq!(
                crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0),
                expected.0
            );
            assert_eq!(
                fixture.vm.as_ref().unwrap().0.inner.lock().status,
                VmStatus::RecoveryRequired
            );
            assert!(
                fixture
                    .vm
                    .as_ref()
                    .unwrap()
                    .0
                    .inner
                    .lock()
                    .shares
                    .is_empty()
            );
            assert_eq!(
                crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0),
                rpc::INVALID_STATE.0
            );
            assert_eq!(
                crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()),
                rpc::INVALID_STATE.0
            );
            assert_eq!(
                crate::WslOpenVmmVmResume(fixture.handle()),
                rpc::INVALID_STATE.0
            );
            assert_eq!(fixture.state.lock().events.len(), 1);
            assert_eq!(crate::WslOpenVmmVmTeardown(fixture.handle()), S_OK.0);
            assert_eq!(crate::WslOpenVmmVmQuit(fixture.handle()), S_OK.0);
            assert_eq!(
                fixture.vm.as_ref().unwrap().0.inner.lock().status,
                VmStatus::RecoveryRequired
            );
            assert_eq!(
                crate::WslOpenVmmVmResume(fixture.handle()),
                rpc::INVALID_STATE.0
            );
            assert_eq!(fixture.state.lock().events.len(), 3);
        }
    }
}

#[test]
fn definite_rejections_preserve_handle_usability() {
    for (code, expected) in [
        (tonic::Code::InvalidArgument, E_INVALIDARG),
        (tonic::Code::OutOfRange, E_INVALIDARG),
        (
            tonic::Code::NotFound,
            HRESULT::from_win32(ERROR_NOT_FOUND.0),
        ),
        (
            tonic::Code::AlreadyExists,
            HRESULT::from_win32(ERROR_ALREADY_EXISTS.0),
        ),
        (tonic::Code::PermissionDenied, E_ACCESSDENIED),
        (
            tonic::Code::Unauthenticated,
            HRESULT::from_win32(ERROR_LOGON_FAILURE.0),
        ),
        (tonic::Code::FailedPrecondition, rpc::INVALID_STATE),
        (tonic::Code::Unimplemented, E_NOTIMPL),
    ] {
        let mut fixture = Fixture::new();
        fixture.state.lock().next_status = Some(code);
        assert_eq!(
            unsafe { crate::WslOpenVmmVmResume(fixture.handle()) },
            expected.0
        );
        assert_eq!(
            fixture.vm.as_ref().unwrap().0.inner.lock().status,
            VmStatus::Active
        );
        assert_eq!(
            unsafe { crate::WslOpenVmmVmResume(fixture.handle()) },
            S_OK.0
        );
        assert_eq!(fixture.state.lock().events.len(), 2);
    }
}

#[test]
fn hung_mutation_and_cleanup_are_bounded() {
    let mut fixture = Fixture::with_timeout(Duration::from_millis(200));
    fixture.state.lock().hang_next = true;
    let start = Instant::now();
    assert_eq!(
        unsafe { crate::WslOpenVmmVmResume(fixture.handle()) },
        rpc::TIMEOUT.0
    );
    assert!(start.elapsed() < Duration::from_secs(2));
    assert_eq!(
        fixture.vm.as_ref().unwrap().0.inner.lock().status,
        VmStatus::RecoveryRequired
    );
    fixture.state.lock().hang_next = true;
    let start = Instant::now();
    assert_eq!(
        unsafe { crate::WslOpenVmmVmTeardown(fixture.handle()) },
        rpc::TIMEOUT.0
    );
    assert!(start.elapsed() < Duration::from_secs(2));
    assert_eq!(
        fixture.vm.as_ref().unwrap().0.inner.lock().status,
        VmStatus::RecoveryRequired
    );
    assert_eq!(unsafe { crate::WslOpenVmmVmQuit(fixture.handle()) }, S_OK.0);
}

#[test]
fn cancellation_interrupts_inflight_rpc_and_allows_cleanup() {
    let fixture = Fixture::new();
    let (send, receive) = std::sync::mpsc::channel();
    fixture.state.lock().observed = Some(send);
    fixture.state.lock().hang_next = true;
    let vm = fixture.vm.as_ref().unwrap();
    std::thread::scope(|scope| {
        let request =
            scope.spawn(|| unsafe { crate::WslOpenVmmVmResume(std::ptr::from_ref(vm).cast_mut()) });
        receive.recv_timeout(Duration::from_secs(2)).unwrap();
        let start = Instant::now();
        assert_eq!(
            unsafe { crate::WslOpenVmmVmCancelRequests(std::ptr::from_ref(vm).cast_mut()) },
            S_OK.0
        );
        assert_eq!(
            request.join().unwrap(),
            windows::Win32::Foundation::E_ABORT.0
        );
        assert!(start.elapsed() < Duration::from_secs(2));
        assert_eq!(vm.0.inner.lock().status, VmStatus::RecoveryRequired);
        assert_eq!(
            unsafe { crate::WslOpenVmmVmResume(std::ptr::from_ref(vm).cast_mut()) },
            rpc::INVALID_STATE.0
        );
        assert_eq!(
            unsafe { crate::WslOpenVmmVmTeardown(std::ptr::from_ref(vm).cast_mut()) },
            S_OK.0
        );
        assert_eq!(
            unsafe { crate::WslOpenVmmVmQuit(std::ptr::from_ref(vm).cast_mut()) },
            S_OK.0
        );
        assert_eq!(fixture.state.lock().events.len(), 3);
    });
}

#[test]
fn cancellation_before_dispatch_sends_no_mutation() {
    let mut fixture = Fixture::new();
    unsafe {
        assert_eq!(crate::WslOpenVmmVmCancelRequests(fixture.handle()), S_OK.0);
        assert_eq!(
            crate::WslOpenVmmVmResume(fixture.handle()),
            rpc::INVALID_STATE.0
        );
    }
    assert_eq!(
        fixture.vm.as_ref().unwrap().0.inner.lock().status,
        VmStatus::RecoveryRequired
    );
    assert!(fixture.state.lock().events.is_empty());
}

#[test]
fn lock_timeout_before_dispatch_does_not_invalidate() {
    let fixture = Fixture::with_timeout(Duration::from_millis(100));
    let vm = fixture.vm.as_ref().unwrap();
    let lock = vm.0.inner.lock();
    std::thread::scope(|scope| {
        let request =
            scope.spawn(|| unsafe { crate::WslOpenVmmVmResume(std::ptr::from_ref(vm).cast_mut()) });
        assert_eq!(request.join().unwrap(), rpc::TIMEOUT.0);
    });
    assert_eq!(lock.status, VmStatus::Active);
    assert!(fixture.state.lock().events.is_empty());
    drop(lock);
    assert_eq!(
        unsafe { crate::WslOpenVmmVmResume(std::ptr::from_ref(vm).cast_mut()) },
        S_OK.0
    );
}

#[test]
fn lock_wait_and_rpc_share_one_deadline() {
    let fixture = Fixture::with_timeout(Duration::from_secs(1));
    fixture.state.lock().hang_next = true;
    let vm = fixture.vm.as_ref().unwrap();
    let lock = vm.0.inner.lock();
    let (send, receive) = std::sync::mpsc::channel();
    std::thread::scope(|scope| {
        let request = scope.spawn(|| {
            send.send(()).unwrap();
            let start = Instant::now();
            let result = unsafe { crate::WslOpenVmmVmResume(std::ptr::from_ref(vm).cast_mut()) };
            (result, start.elapsed())
        });
        receive.recv_timeout(Duration::from_secs(2)).unwrap();
        std::thread::sleep(Duration::from_millis(700));
        drop(lock);
        let (result, elapsed) = request.join().unwrap();
        assert_eq!(result, rpc::TIMEOUT.0);
        assert!(elapsed < Duration::from_millis(1400), "{elapsed:?}");
    });
    assert_eq!(fixture.state.lock().events.len(), 1);
    assert_eq!(vm.0.inner.lock().status, VmStatus::RecoveryRequired);
}

#[test]
fn create_failure_preserves_ownership_but_uncertain_outcome_blocks_retry() {
    for (code, expected) in [
        (tonic::Code::InvalidArgument, E_INVALIDARG),
        (tonic::Code::Internal, E_FAIL),
    ] {
        let fixture = Fixture::new();
        let path = wide(fixture.directory.socket_path().to_str().unwrap());
        let mut config = std::ptr::null_mut();
        let mut vm = std::ptr::null_mut();
        unsafe {
            assert_eq!(crate::WslOpenVmmCreateConfig(&mut config), S_OK.0);
            let original = config;
            assert_eq!(
                crate::WslOpenVmmCreateVm(&mut config, path.as_ptr(), 0, &mut vm),
                E_INVALIDARG.0
            );
            fixture.state.lock().next_status = Some(code);
            assert_eq!(
                crate::WslOpenVmmCreateVm(&mut config, path.as_ptr(), 2000, &mut vm),
                expected.0
            );
            assert_eq!(config, original);
            assert!(vm.is_null());
            if code == tonic::Code::Internal {
                assert_eq!(
                    crate::WslOpenVmmCreateVm(&mut config, path.as_ptr(), 2000, &mut vm),
                    rpc::INVALID_STATE.0
                );
                assert_eq!(fixture.state.lock().events.len(), 1);
                assert_eq!(config, original);
                assert!(vm.is_null());
                crate::WslOpenVmmDestroyConfig(config);
            } else {
                assert_eq!(
                    crate::WslOpenVmmCreateVm(&mut config, path.as_ptr(), 2000, &mut vm),
                    S_OK.0
                );
                assert!(config.is_null());
                assert!(!vm.is_null());
                crate::WslOpenVmmDestroyVm(vm);
            }
        }
    }
}

#[test]
fn connection_failure_before_create_is_retryable() {
    let fixture = Fixture::new();
    let directory = SocketDirectory::new();
    let mut config = VmConfigBuilder::new();
    let timeout = Duration::from_millis(100);
    let result = config.create_vm(
        directory.socket_path().to_string_lossy().into_owned(),
        timeout,
        Instant::now() + timeout,
    );
    assert_eq!(result.err().unwrap(), rpc::TIMEOUT);
    assert_eq!(config.status, VmConfigStatus::Ready);
    let timeout = Duration::from_secs(2);
    assert!(
        config
            .create_vm(
                fixture
                    .directory
                    .socket_path()
                    .to_string_lossy()
                    .into_owned(),
                timeout,
                Instant::now() + timeout
            )
            .is_ok()
    );
}

#[test]
fn invalid_socket_path_is_not_reported_as_timeout() {
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let error = runtime
        .block_on(af_unix::connect_channel(
            std::path::PathBuf::from("x".repeat(200)),
            Duration::from_secs(1),
        ))
        .unwrap_err();
    assert_eq!(rpc::io_error_to_hresult(&error), E_INVALIDARG);
}

#[test]
fn create_timeout_invalidates_config_and_does_not_replay() {
    let fixture = Fixture::new();
    fixture.state.lock().hang_next = true;
    let mut config = VmConfigBuilder::new();
    let path = fixture
        .directory
        .socket_path()
        .to_string_lossy()
        .into_owned();
    let timeout = Duration::from_millis(200);
    assert_eq!(
        config
            .create_vm(path.clone(), timeout, Instant::now() + timeout)
            .err()
            .unwrap(),
        rpc::TIMEOUT
    );
    assert_eq!(config.status, VmConfigStatus::CreationOutcomeUnknown);
    assert_eq!(
        config
            .create_vm(path, timeout, Instant::now() + timeout)
            .err()
            .unwrap(),
        rpc::INVALID_STATE
    );
    assert_eq!(fixture.state.lock().events.len(), 1);
}

#[test]
fn wrong_protocol_and_silent_af_unix_peers_fail_within_deadline() {
    use std::io::{Read, Write};

    for garbage in [false, true] {
        let directory = SocketDirectory::new();
        let listener = directory.listen();
        let (shutdown, receive) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(3);
            let (socket, _) = loop {
                match listener.accept() {
                    Ok(connection) => break connection,
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        assert!(Instant::now() < deadline, "client did not connect");
                        std::thread::sleep(Duration::from_millis(2));
                    }
                    Err(error) => panic!("{error}"),
                }
            };
            socket.set_nonblocking(false).unwrap();
            let mut stream: std::net::TcpStream = socket.into();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            let mut preface = [0; 24];
            stream.read_exact(&mut preface).unwrap();
            assert_eq!(&preface, b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
            if garbage {
                stream
                    .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
                    .unwrap();
            }
            // Hold the peer open until the client's result, not a peer-side timeout.
            let _ = receive.recv_timeout(Duration::from_secs(3));
        });
        let mut config = VmConfigBuilder::new();
        let timeout = Duration::from_millis(300);
        let start = Instant::now();
        let result = config.create_vm(
            directory.socket_path().to_string_lossy().into_owned(),
            timeout,
            start + timeout,
        );
        let elapsed = start.elapsed();
        let _ = shutdown.send(());
        thread.join().unwrap();
        let error = result.err().expect("non-gRPC peer must not create a VM");
        assert!(elapsed < Duration::from_secs(2), "{elapsed:?}");
        if !garbage {
            assert_eq!(error, rpc::TIMEOUT);
        }
    }
}

#[test]
fn connection_retries_until_af_unix_listener_appears() {
    let directory = SocketDirectory::new();
    let path = directory.socket_path();
    let (shutdown, receive) = std::sync::mpsc::channel();
    let thread = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(100));
        let listener =
            socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::STREAM, None).unwrap();
        listener
            .bind(&socket2::SockAddr::unix(path).unwrap())
            .unwrap();
        listener.listen(16).unwrap();
        listener.set_nonblocking(true).unwrap();
        let deadline = Instant::now() + Duration::from_secs(3);
        let connection = loop {
            match listener.accept() {
                Ok((socket, _)) => break socket,
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                    assert!(Instant::now() < deadline, "client did not retry connection");
                    std::thread::sleep(Duration::from_millis(2));
                }
                Err(error) => panic!("{error}"),
            }
        };
        let _ = receive.recv_timeout(Duration::from_secs(3));
        drop(connection);
    });
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let result = runtime.block_on(af_unix::connect_channel(
        directory.socket_path(),
        Duration::from_secs(2),
    ));
    let _ = shutdown.send(());
    thread.join().unwrap();
    assert!(result.is_ok(), "{result:?}");
}
