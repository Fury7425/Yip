#![warn(clippy::pedantic)]
#![allow(clippy::module_name_repetitions)]

//! audio-core: WASAPI capture, lock-free ring, WAV writer.
//! Public surface lives in [`ffi`]. All other modules are internal.

pub mod capture;
pub mod devices;
mod error;
pub mod ffi;
pub mod ring;
pub mod writer;

pub use error::YipError;
