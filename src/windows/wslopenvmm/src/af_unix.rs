// Copyright (C) Microsoft Corporation. All rights reserved.

use std::io;
use std::path::{Path, PathBuf};
use std::time::Duration;

use hyper_util::rt::TokioIo;
use socket2::{Domain, SockAddr, Socket, Type};
use tokio::net::TcpStream;
use tonic::transport::{Channel, Endpoint};
use tower::service_fn;

/// Connects a Tonic channel to the OpenVMM AF_UNIX listener.
pub async fn connect_channel(
    socket_path: PathBuf,
    timeout: Duration,
) -> Result<Channel, tonic::transport::Error> {
    Endpoint::from_static("http://[::]:50051")
        .connect_timeout(timeout)
        .connect_with_connector(service_fn(move |_| {
            let socket_path = socket_path.clone();
            async move { open_socket(&socket_path, timeout).await.map(TokioIo::new) }
        }))
        .await
}

async fn open_socket(path: &Path, timeout: Duration) -> io::Result<TcpStream> {
    let address = SockAddr::unix(path)?;
    tokio::time::timeout(timeout, async {
        loop {
            match connect_socket(&address).await {
                Ok(stream) => return Ok(stream),
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::NotFound | io::ErrorKind::ConnectionRefused
                    ) => {}
                Err(error) => return Err(error),
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await
    .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "timed out connecting to OpenVMM"))?
}

/// Registers the Winsock AF_UNIX socket with Tokio's Windows socket reactor.
/// The socket remains AF_UNIX; TCP-specific options and address queries must
/// not be used on the returned stream.
async fn connect_socket(address: &SockAddr) -> io::Result<TcpStream> {
    let socket = Socket::new(Domain::UNIX, Type::STREAM, None)?;
    socket.set_nonblocking(true)?;
    match socket.connect(address) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
        Err(error) => return Err(error),
    }

    let stream = TcpStream::from_std(socket.into())?;
    stream.writable().await?;
    if let Some(error) = stream.take_error()? {
        return Err(error);
    }
    Ok(stream)
}