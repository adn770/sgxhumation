// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Write doc/isa/sim-coverage.json: what the simulator models of each form,
//! against the probe vectors of data/vectors (sgx_sim::vectors).

fn main() {
    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../doc/isa/sim-coverage.json");
    std::fs::write(&out, sgx_sim::vectors::coverage_json(&sgx_sim::vectors::data())).unwrap();
    eprintln!("wrote {}", out.display());
}
