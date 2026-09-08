use super::*;
use parking_lot::Mutex;
use std::sync::Arc;
use tonic::{Response, Status};

#[derive(Debug)]
enum Event {
    Port(ModifyResourceRequest),
    Add(vmservice::AddVpciDeviceRequest),
    Remove(vmservice::RemoveVpciDeviceRequest),
}

#[derive(Default)]
struct State {
    events: Vec<Event>,
    fail_next: bool,
}

#[derive(Clone)]
struct Server(Arc<Mutex<State>>);

impl Server {
    fn record(&self, event: Event) -> Result<Response<()>, Status> {
        let mut state = self.0.lock();
        state.events.push(event);
        if std::mem::take(&mut state.fail_next) {
            Err(Status::internal("injected failure"))
        } else {
            Ok(Response::new(()))
        }
    }
}

#[tonic::async_trait]
impl vmservice::vm_server::Vm for Server {
    async fn create_vm(&self, _: Request<CreateVmRequest>) -> Result<Response<()>, Status> {
        Ok(Response::new(()))
    }
    async fn teardown_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Ok(Response::new(()))
    }
    async fn pause_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn resume_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn wait_vm(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn capabilities_vm(&self, _: Request<()>) -> Result<Response<vmservice::CapabilitiesVmResponse>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn properties_vm(&self, _: Request<vmservice::PropertiesVmRequest>) -> Result<Response<vmservice::PropertiesVmResponse>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn modify_resource(&self, request: Request<ModifyResourceRequest>) -> Result<Response<()>, Status> {
        self.record(Event::Port(request.into_inner()))
    }
    async fn add_pcie_device(&self, _: Request<vmservice::AddPcieDeviceRequest>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn remove_pcie_device(&self, _: Request<vmservice::RemovePcieDeviceRequest>) -> Result<Response<()>, Status> {
        Err(Status::unimplemented("unused"))
    }
    async fn add_vpci_device(&self, request: Request<vmservice::AddVpciDeviceRequest>) -> Result<Response<()>, Status> {
        self.record(Event::Add(request.into_inner()))
    }
    async fn remove_vpci_device(&self, request: Request<vmservice::RemoveVpciDeviceRequest>) -> Result<Response<()>, Status> {
        self.record(Event::Remove(request.into_inner()))
    }
    async fn quit(&self, _: Request<()>) -> Result<Response<()>, Status> {
        Ok(Response::new(()))
    }
}

struct Fixture {
    vm: Option<crate::WslOpenVmmVm>,
    state: Arc<Mutex<State>>,
    shutdown: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Fixture {
    fn new() -> Self {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let address = listener.local_addr().unwrap();
        let state = Arc::new(Mutex::new(State::default()));
        let server = Server(state.clone());
        let (shutdown, receive) = tokio::sync::oneshot::channel();
        let thread = std::thread::spawn(move || {
            tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap().block_on(async {
                let incoming = tokio_stream::wrappers::TcpListenerStream::new(
                    tokio::net::TcpListener::from_std(listener).unwrap(),
                );
                tonic::transport::Server::builder()
                    .add_service(vmservice::vm_server::VmServer::new(server))
                    .serve_with_incoming_shutdown(incoming, async { let _ = receive.await; })
                    .await.unwrap();
            });
        });
        let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        let client = runtime.block_on(VmClient::connect(format!("http://{address}"))).unwrap();
        Self {
            vm: Some(crate::WslOpenVmmVm(Mutex::new(VmHandle {
                inner: VmHandleInner { runtime, client, timeout: Duration::from_secs(5), shares: BTreeMap::new() },
            }))),
            state,
            shutdown: Some(shutdown),
            thread: Some(thread),
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
    for (family, address) in [(i32::from(AF_INET), "127.0.0.1"), (i32::from(AF_INET6), "::1")] {
        for tcp in [0, 1] {
            unsafe {
                assert_eq!(crate::WslOpenVmmVmBindPort(fixture.handle(), 8080, 80, tcp, family), S_OK.0);
                assert_eq!(crate::WslOpenVmmVmUnbindPort(fixture.handle(), 8080, 80, tcp, family), S_OK.0);
            }
            let events = std::mem::take(&mut fixture.state.lock().events);
            assert_eq!(events.len(), 2);
            for (event, operation) in events.into_iter().zip([ModifyType::Update, ModifyType::Remove]) {
                let Event::Port(request) = event else { panic!("expected port request") };
                assert_eq!(request.r#type, operation as i32);
                let Some(vmservice::modify_resource_request::Resource::NicConfig(nic)) = request.resource else { panic!("expected NIC") };
                let Some(vmservice::nic_config::Backend::Consomme(backend)) = nic.backend else { panic!("expected Consomme") };
                assert_eq!(backend.ports, vec![PortConfig {
                    host_port: 8080, guest_port: 80,
                    protocol: if tcp != 0 { vmservice::IpProtocol::Tcp } else { vmservice::IpProtocol::Udp } as i32,
                    host_address: address.to_string(),
                }]);
            }
        }
    }
    unsafe {
        assert_eq!(crate::WslOpenVmmVmBindPort(fixture.handle(), 8080, 80, 1, 0), E_INVALIDARG.0);
        assert_eq!(crate::WslOpenVmmVmUnbindPort(fixture.handle(), 8080, 80, 1, 0), E_INVALIDARG.0);
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
        assert_ne!(crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 1), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 1), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0), HRESULT::from_win32(ERROR_ALREADY_EXISTS.0).0);
    }
    let events = std::mem::take(&mut fixture.state.lock().events);
    assert_eq!(events.len(), 2);
    let Event::Add(request) = &events[1] else { panic!("expected add") };
    assert_ne!(request.instance_id, String::new());
    let instance_id = request.instance_id.clone();
    let Some(vmservice::pcie_device_kind::Kind::Virtio(device)) = &request.device.as_ref().unwrap().kind else { panic!("expected virtio") };
    let Some(vmservice::virtio_device::Kind::Fs(fs)) = &device.kind else { panic!("expected filesystem") };
    assert_eq!(fs.tag, "share");
    assert_eq!(fs.root_path, r"C:\share");
    assert!(fs.read_only);
    fixture.state.lock().fail_next = true;
    unsafe {
        assert_ne!(crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()), HRESULT::from_win32(ERROR_NOT_FOUND.0).0);
    }
    let events = std::mem::take(&mut fixture.state.lock().events);
    assert_eq!(events.len(), 2);
    for event in events {
        let Event::Remove(request) = event else { panic!("expected remove") };
        assert_eq!(request.instance_id, instance_id);
    }
    unsafe {
        assert_eq!(crate::WslOpenVmmVmAddShare(fixture.handle(), tag.as_ptr(), path.as_ptr(), 0), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmTeardown(fixture.handle()), S_OK.0);
        assert_eq!(crate::WslOpenVmmVmRemoveShare(fixture.handle(), tag.as_ptr()), HRESULT::from_win32(ERROR_NOT_FOUND.0).0);
    }
    let events = std::mem::take(&mut fixture.state.lock().events);
    let Event::Add(request) = &events[0] else { panic!("expected add") };
    assert_ne!(request.instance_id, instance_id);
    let Some(vmservice::pcie_device_kind::Kind::Virtio(device)) = &request.device.as_ref().unwrap().kind else { panic!("expected virtio") };
    let Some(vmservice::virtio_device::Kind::Fs(fs)) = &device.kind else { panic!("expected filesystem") };
    assert!(!fs.read_only);
}