#![warn(clippy::pedantic)]
// Project-wide pedantic exemptions. These cover lint families that fight
// FFI-heavy WASAPI code or punish standard idioms without buying safety.
// Re-evaluate during M7 polish.
#![allow(
    clippy::cast_lossless,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::doc_markdown,
    clippy::missing_errors_doc,
    clippy::missing_panics_doc,
    clippy::missing_safety_doc,
    clippy::module_name_repetitions,
    clippy::must_use_candidate,
    clippy::needless_pass_by_value,
    clippy::similar_names,
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::unreadable_literal
)]

//! audio-core: WASAPI capture, lock-free ring, WAV writer.
//! Public surface lives in [`ffi`]. All other modules are internal.

pub mod capture;
pub mod devices;
mod error;
pub mod ffi;
pub mod ring;
pub mod writer;

pub use error::YipError;
