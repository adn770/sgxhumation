// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! SGX545 simulator: one USSE instance at a time, over the decoder's form table.
//!
//! What a form does is in the ISA description (`isa/usse/*.toml`: each form's
//! `op` and its fields' roles). Programs are decoded once per word and run from
//! that cache. Memory goes through a [`Bus`]. No timing, caches or
//! fixed-function units. Hardware is the reference for every semantic here:
//! `tests/vectors.rs` runs the recorded instruction vectors
//! (design/sgx545_vectors.md). Anything not established yet stops the program
//! with [`Error::Unsupported`] rather than being guessed.

mod usse;

pub use usse::{Bus, Error, Machine, Memory, Program};
