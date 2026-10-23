// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Write doc/isa/sim-coverage.json: what the simulator models of each form,
//! against the probe vectors of the offline corpus (sgx_sim::vectors).

fn main() {
    let Some(corpus) = sgx_sim::vectors::corpus() else {
        eprintln!("no corpus (set SGX_CORPUS)");
        std::process::exit(1);
    };
    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../doc/isa/sim-coverage.json");
    std::fs::write(&out, sgx_sim::vectors::coverage_json(&corpus)).unwrap();
    eprintln!("wrote {}", out.display());
}
