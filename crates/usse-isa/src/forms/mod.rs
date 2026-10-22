// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The instruction families, one module each (tools/usse-dis/forms/README.md):
//! their operand kinds, fields and table entries. Clean-room like the rest of
//! the table: layouts come from probing the refdisasm545 binary and are held
//! to 0 wrong by the random cross-checks.

pub(crate) mod float;
pub(crate) mod intpack;
pub(crate) mod memctl;
pub(crate) mod misc;
