// Copyright (C) Microsoft Corporation. All rights reserved.

use std::error::Error;
use std::future::Future;
use std::io;
use std::time::Instant;

use tokio::sync::watch;
use tonic::Code;
use windows::Win32::Foundation::{
    E_ABORT, E_ACCESSDENIED, E_FAIL, E_INVALIDARG, E_NOTIMPL, ERROR_ALREADY_EXISTS,
    ERROR_CONNECTION_ABORTED, ERROR_INVALID_DATA, ERROR_INVALID_STATE, ERROR_LOGON_FAILURE,
    ERROR_NOT_ENOUGH_MEMORY, ERROR_NOT_FOUND, ERROR_RETRY, WAIT_TIMEOUT,
};
use windows::core::HRESULT;

pub const INVALID_STATE: HRESULT = HRESULT::from_win32(ERROR_INVALID_STATE.0);
pub const TIMEOUT: HRESULT = HRESULT::from_win32(WAIT_TIMEOUT.0);

pub struct RpcFailure {
    pub result: HRESULT,
    pub uncertain: bool,
}

pub async fn execute<T>(
    deadline: Instant,
    cancellation: Option<watch::Receiver<bool>>,
    operation: impl Future<Output = Result<T, tonic::Status>>,
) -> Result<T, RpcFailure> {
    // A closed sender also cancels the operation; no lifecycle owner is required here.
    let cancellation = async {
        if let Some(mut cancellation) = cancellation {
            let _ = cancellation.wait_for(|cancelled| *cancelled).await;
        } else {
            std::future::pending::<()>().await;
        }
    };
    // Prefer the local deadline when the peer's cancellation arrives in the same poll.
    // This also bounds peers that ignore grpc-timeout.
    tokio::select! {
        biased;
        _ = tokio::time::sleep_until(deadline.into()) =>
            Err(RpcFailure { result: TIMEOUT, uncertain: true }),
        _ = cancellation =>
            Err(RpcFailure { result: E_ABORT, uncertain: true }),
        result = operation => result.map_err(failure_from_status),
    }
}

fn failure_from_status(status: tonic::Status) -> RpcFailure {
    let mut source = status.source();
    while let Some(error) = source {
        // Tonic reports its local request timeout with Code::Cancelled.
        if error.is::<tonic::TimeoutExpired>() {
            return RpcFailure {
                result: TIMEOUT,
                uncertain: true,
            };
        }
        source = error.source();
    }
    RpcFailure {
        result: status_to_hresult(status.code()),
        uncertain: !is_definite_rejection(status.code()),
    }
}

fn status_to_hresult(code: Code) -> HRESULT {
    match code {
        Code::Ok => windows::Win32::Foundation::S_OK,
        Code::Cancelled => E_ABORT,
        Code::InvalidArgument | Code::OutOfRange => E_INVALIDARG,
        Code::DeadlineExceeded => TIMEOUT,
        Code::NotFound => HRESULT::from_win32(ERROR_NOT_FOUND.0),
        Code::AlreadyExists => HRESULT::from_win32(ERROR_ALREADY_EXISTS.0),
        Code::PermissionDenied => E_ACCESSDENIED,
        Code::Unauthenticated => HRESULT::from_win32(ERROR_LOGON_FAILURE.0),
        Code::ResourceExhausted => HRESULT::from_win32(ERROR_NOT_ENOUGH_MEMORY.0),
        Code::FailedPrecondition => INVALID_STATE,
        Code::Aborted => HRESULT::from_win32(ERROR_RETRY.0),
        Code::Unimplemented => E_NOTIMPL,
        Code::Unavailable => HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0),
        Code::DataLoss => HRESULT::from_win32(ERROR_INVALID_DATA.0),
        Code::Unknown | Code::Internal => E_FAIL,
    }
}

fn is_definite_rejection(code: Code) -> bool {
    // Only validation/authorization rejections are safe to correct and resubmit.
    // Internal, aborted, resource exhaustion and transport errors may follow a mutation.
    matches!(
        code,
        Code::InvalidArgument
            | Code::OutOfRange
            | Code::NotFound
            | Code::AlreadyExists
            | Code::PermissionDenied
            | Code::Unauthenticated
            | Code::FailedPrecondition
            | Code::Unimplemented
    )
}

pub fn io_error_to_hresult(error: &io::Error) -> HRESULT {
    match error.kind() {
        io::ErrorKind::TimedOut => TIMEOUT,
        io::ErrorKind::PermissionDenied => E_ACCESSDENIED,
        io::ErrorKind::InvalidInput => E_INVALIDARG,
        _ => match error.raw_os_error() {
            Some(code) => HRESULT::from_win32(code as u32),
            None => HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn execution_accepts_cancellation_without_vm_state() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        for cancelled in [true, false] {
            let (send, receive) = watch::channel(cancelled);
            if !cancelled {
                drop(send);
            }
            let result = runtime.block_on(execute::<()>(
                Instant::now() + std::time::Duration::from_secs(1),
                Some(receive),
                std::future::pending(),
            ));
            let error = result.unwrap_err();
            assert_eq!(error.result, E_ABORT);
            assert!(error.uncertain);
        }
    }

    #[test]
    fn local_deadline_does_not_depend_on_peer_timeout_support() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let start = Instant::now();
        let result = runtime.block_on(execute::<()>(
            start + std::time::Duration::from_millis(50),
            None,
            std::future::pending(),
        ));
        let error = result.unwrap_err();
        assert_eq!(error.result, TIMEOUT);
        assert!(error.uncertain);
        assert!(start.elapsed() < std::time::Duration::from_secs(2));
    }

    #[test]
    fn status_and_io_errors_have_explicit_hresult_categories() {
        for (code, expected) in [
            (Code::Ok, windows::Win32::Foundation::S_OK),
            (Code::Cancelled, E_ABORT),
            (Code::Unknown, E_FAIL),
            (Code::InvalidArgument, E_INVALIDARG),
            (Code::DeadlineExceeded, TIMEOUT),
            (Code::NotFound, HRESULT::from_win32(ERROR_NOT_FOUND.0)),
            (
                Code::AlreadyExists,
                HRESULT::from_win32(ERROR_ALREADY_EXISTS.0),
            ),
            (Code::PermissionDenied, E_ACCESSDENIED),
            (
                Code::ResourceExhausted,
                HRESULT::from_win32(ERROR_NOT_ENOUGH_MEMORY.0),
            ),
            (Code::FailedPrecondition, INVALID_STATE),
            (Code::Aborted, HRESULT::from_win32(ERROR_RETRY.0)),
            (Code::OutOfRange, E_INVALIDARG),
            (Code::Unimplemented, E_NOTIMPL),
            (Code::Internal, E_FAIL),
            (
                Code::Unavailable,
                HRESULT::from_win32(ERROR_CONNECTION_ABORTED.0),
            ),
            (Code::DataLoss, HRESULT::from_win32(ERROR_INVALID_DATA.0)),
            (
                Code::Unauthenticated,
                HRESULT::from_win32(ERROR_LOGON_FAILURE.0),
            ),
        ] {
            assert_eq!(status_to_hresult(code), expected);
        }
        assert_eq!(
            io_error_to_hresult(&io::Error::from(io::ErrorKind::TimedOut)),
            TIMEOUT
        );
        assert_eq!(
            io_error_to_hresult(&io::Error::from(io::ErrorKind::InvalidInput)),
            E_INVALIDARG
        );
        assert_eq!(
            io_error_to_hresult(&io::Error::from(io::ErrorKind::PermissionDenied)),
            E_ACCESSDENIED
        );
        assert_eq!(
            io_error_to_hresult(&io::Error::from_raw_os_error(10054)),
            HRESULT::from_win32(10054)
        );
    }
}
