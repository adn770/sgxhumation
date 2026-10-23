// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! pysgx: the Rust tooling for the Python scripts (crates/README.md).
//!
//! - `usse_decode`, `usse_encode`, `pds_decode`, `pds_encode`: one word, one text.
//! - `assemble(source, file, core)`: sgx-asm in-process; sections with their
//!   images, labels and patch records, as `sgx-asm -d` writes them to map.json.
//! - `Program`, `Machine`: the USSE simulator. A run reads memory, SGX registers
//!   and task emissions through a bus, any Python object with the methods
//!   `load32(addr)`, `store32(addr, value)`, `reg_read(index)`,
//!   `reg_write(index, value)`, `emit(text, (a, b, c))`; a missing one stops the
//!   program as the simulator's own buses do.

use pyo3::call::PyCallArgs;
use pyo3::create_exception;
use pyo3::exceptions::{PyException, PyValueError};
use pyo3::prelude::*;
use pyo3::types::{PyBytes, PyDict, PyList};
use sgx_sim::{Bus, Error};
use sgx_tools::asm;
use usse_isa::Core;

create_exception!(pysgx, AsmError, PyException, "sgx-asm's messages, one per line.");
create_exception!(
    pysgx,
    SimError,
    PyException,
    "The simulator stopped: args are (kind, message), kind one of unsupported, fault, pc, steps."
);

fn core(c: u32) -> PyResult<Core> {
    match c {
        540 => Ok(Core::Sgx540),
        545 => Ok(Core::Sgx545),
        _ => Err(PyValueError::new_err("core must be 540 or 545")),
    }
}

/// The text of a USSE instruction word, as usse-dis prints it.
#[pyfunction]
#[pyo3(signature = (word, core = 545))]
fn usse_decode(word: u64, core: u32) -> PyResult<String> {
    Ok(usse_isa::decode(word, self::core(core)?).text)
}

/// The word of a USSE instruction text, or None.
#[pyfunction]
#[pyo3(signature = (text, core = 545))]
fn usse_encode(text: &str, core: u32) -> PyResult<Option<u64>> {
    Ok(usse_isa::encode(text, self::core(core)?))
}

/// The text of a PDS instruction word, as pds-dis prints it.
#[pyfunction]
fn pds_decode(word: u32) -> String {
    pds_isa::decode(word).text
}

/// The word of a PDS instruction text, or None.
#[pyfunction]
fn pds_encode(text: &str) -> Option<u32> {
    pds_isa::encode(text)
}

#[derive(FromPyObject)]
enum Source {
    Text(String),
    Bytes(Vec<u8>),
}

/// Assemble an sgx-asm source: a dict with "sections" (name, isa, id, image,
/// data), "symbols" (name, section, index, offset) and "patches" (section, id,
/// offset, encoding, kind, target, addend), as in sgx-asm's map.json. `file`
/// names the source in messages; .include is relative to its directory.
#[pyfunction]
#[pyo3(signature = (source, file = "<string>", core = 545))]
fn assemble<'py>(py: Python<'py>, source: Source, file: &str, core: u32) -> PyResult<Bound<'py, PyDict>> {
    let text = match source {
        Source::Text(s) => s.into_bytes(),
        Source::Bytes(b) => b,
    };
    let p = asm::assemble_program(&text, file, self::core(core)?, false);
    if p.errors > 0 {
        return Err(AsmError::new_err(String::from_utf8_lossy(&p.messages).trim_end().to_string()));
    }
    let sections = PyList::empty(py);
    for s in &p.sections {
        let d = PyDict::new(py);
        let image: Vec<u8> = s.words.iter().flat_map(|w| w.to_le_bytes()[..s.isa.width].to_vec()).collect();
        d.set_item("name", &s.name)?;
        d.set_item("isa", s.isa_name())?;
        d.set_item("id", s.id)?;
        d.set_item("image", PyBytes::new(py, &image))?;
        d.set_item("data", s.ndata)?;
        sections.append(d)?;
    }
    let symbols = PyList::empty(py);
    for y in &p.symbols {
        let d = PyDict::new(py);
        d.set_item("name", &y.name)?;
        d.set_item("section", &p.sections[y.section].name)?;
        d.set_item("index", y.index)?;
        d.set_item("offset", y.offset)?;
        symbols.append(d)?;
    }
    let patches = PyList::empty(py);
    for r in &p.patches {
        let d = PyDict::new(py);
        let s = &p.sections[r.section];
        d.set_item("section", &s.name)?;
        d.set_item("id", s.id)?;
        d.set_item("offset", r.offset)?;
        d.set_item("encoding", asm::ENCODINGS[r.encoding])?;
        d.set_item("kind", asm::KINDS[r.kind].name)?;
        d.set_item("target", r.target)?;
        d.set_item("addend", r.addend)?;
        patches.append(d)?;
    }
    let out = PyDict::new(py);
    out.set_item("sections", sections)?;
    out.set_item("symbols", symbols)?;
    out.set_item("patches", patches)?;
    Ok(out)
}

/// A USSE program, decoded once.
#[pyclass(unsendable)]
struct Program {
    p: sgx_sim::Program,
    len: usize,
}

#[pymethods]
impl Program {
    #[new]
    #[pyo3(signature = (words, core = 545))]
    fn new(words: Vec<u64>, core: u32) -> PyResult<Self> {
        Ok(Program { p: sgx_sim::Program::new(&words, self::core(core)?), len: words.len() })
    }

    fn __len__(&self) -> usize {
        self.len
    }

    /// The text of the instruction at `pc`.
    fn text(&self, pc: usize) -> Option<String> {
        sgx_sim::Machine::text(&self.p, pc).map(String::from)
    }
}

/// The bus a run goes through: the Python object's methods.
struct PyBus<'a, 'py> {
    obj: &'a Bound<'py, PyAny>,
    /// the first exception a method raised; the run stops on it
    err: Option<PyErr>,
}

impl<'py> PyBus<'_, 'py> {
    fn has(&self, name: &str) -> bool {
        self.obj.hasattr(name).unwrap_or(false)
    }

    /// Call a method; an exception is kept and stops the program.
    fn call<T: FromPyObjectOwned<'py>>(
        &mut self,
        name: &str,
        args: impl PyCallArgs<'py>,
    ) -> Result<T, Error> {
        match self.obj.call_method1(name, args).and_then(|v| v.extract::<T>().map_err(Into::into)) {
            Ok(v) => Ok(v),
            Err(e) => {
                let why = format!("bus.{name} raised {e}");
                self.err.get_or_insert(e);
                Err(Error::Unsupported { pc: 0, text: String::new(), why })
            }
        }
    }
}

fn no(what: String) -> Error {
    Error::Unsupported { pc: 0, text: String::new(), why: what }
}

impl Bus for PyBus<'_, '_> {
    fn load32(&mut self, addr: u32) -> Result<u32, Error> {
        if !self.has("load32") {
            return Err(Error::Fault(addr));
        }
        // None: nothing there
        let v: Option<u32> = self.call("load32", (addr,))?;
        v.ok_or(Error::Fault(addr))
    }

    fn store32(&mut self, addr: u32, v: u32) -> Result<(), Error> {
        if !self.has("store32") {
            return Err(Error::Fault(addr));
        }
        let ok: Option<bool> = self.call("store32", (addr, v))?;
        if ok == Some(false) { Err(Error::Fault(addr)) } else { Ok(()) }
    }

    fn reg_read(&mut self, index: u32) -> Result<u32, Error> {
        if !self.has("reg_read") {
            return Err(no(format!("ldr #{index}: no SGX registers on this bus")));
        }
        self.call("reg_read", (index,))
    }

    fn reg_write(&mut self, index: u32, v: u32) -> Result<(), Error> {
        if !self.has("reg_write") {
            return Err(no(format!("str #{index}: no SGX registers on this bus")));
        }
        let _: Option<bool> = self.call("reg_write", (index, v))?;
        Ok(())
    }

    fn emit(&mut self, text: &str, srcs: [u32; 3]) -> Result<(), Error> {
        if !self.has("emit") {
            return Err(no(format!("{text}: no task emission on this bus")));
        }
        let _: Option<bool> = self.call("emit", (text.to_string(), (srcs[0], srcs[1], srcs[2])))?;
        Ok(())
    }
}

fn sim_error(e: Error) -> PyErr {
    let kind = match e {
        Error::Unsupported { .. } => "unsupported",
        Error::Fault(_) => "fault",
        Error::PcOutOfRange(_) => "pc",
        Error::StepLimit => "steps",
    };
    SimError::new_err((kind, e.to_string()))
}

/// One USSE instance. Registers a program reads must have been written, by
/// the program or with `set` (the simulator stops on anything else).
#[pyclass(unsendable)]
struct Machine {
    m: sgx_sim::Machine,
}

/// A bank's registers and its written-bits.
fn bank<'m>(m: &'m mut sgx_sim::Machine, name: &str) -> PyResult<(&'m mut [u32], &'m mut u128)> {
    let w = &mut m.written;
    Ok(match name {
        "r" => (&mut m.r[..], &mut w.r),
        "o" => (&mut m.o[..], &mut w.o),
        "pa" => (&mut m.pa[..], &mut w.pa),
        "sa" => (&mut m.sa[..], &mut w.sa),
        _ => return Err(PyValueError::new_err(format!("no bank '{name}' (r, o, pa, sa, i)"))),
    })
}

#[pymethods]
impl Machine {
    #[new]
    #[pyo3(signature = (core = 545, near = false))]
    fn new(core: u32, near: bool) -> PyResult<Self> {
        let mut m = sgx_sim::Machine { core: self::core(core)?, ..Default::default() };
        m.near = near;
        Ok(Machine { m })
    }

    /// Register `n` of bank r, o, pa, sa or i.
    fn get(&mut self, bank: &str, n: usize) -> PyResult<u32> {
        if bank == "i" {
            return self.m.i.get(n).copied().ok_or_else(|| PyValueError::new_err("i0-i3"));
        }
        let (regs, _) = self::bank(&mut self.m, bank)?;
        regs.get(n).copied().ok_or_else(|| PyValueError::new_err(format!("{bank}{n}: no such register")))
    }

    /// Set register `n` of a bank, as the job would before the task starts.
    fn set(&mut self, bank: &str, n: usize, value: u32) -> PyResult<()> {
        if bank == "i" {
            if n >= 4 {
                return Err(PyValueError::new_err("i0-i3"));
            }
            self.m.i[n] = value;
            self.m.written.i |= 1 << n;
            return Ok(());
        }
        let (regs, written) = self::bank(&mut self.m, bank)?;
        let Some(r) = regs.get_mut(n) else {
            return Err(PyValueError::new_err(format!("{bank}{n}: no such register")));
        };
        *r = value;
        *written |= 1u128 << n;
        Ok(())
    }

    /// Predicate `n` (0-3).
    fn pred(&self, n: usize) -> PyResult<bool> {
        self.m.p.get(n).copied().ok_or_else(|| PyValueError::new_err("p0-p3"))
    }

    fn set_pred(&mut self, n: usize, value: bool) -> PyResult<()> {
        *self.m.p.get_mut(n).ok_or_else(|| PyValueError::new_err("p0-p3"))? = value;
        Ok(())
    }

    #[getter]
    fn pc(&self) -> usize {
        self.m.pc
    }

    #[setter]
    fn set_pc(&mut self, pc: usize) {
        self.m.pc = pc;
    }

    #[getter]
    fn steps(&self) -> usize {
        self.m.steps
    }

    /// Run from `pc` until an instruction with `.end` retires; raises SimError
    /// when the simulator stops, or what a bus method raised.
    #[pyo3(signature = (program, bus, max_steps = 100_000))]
    fn run(&mut self, program: &Program, bus: &Bound<'_, PyAny>, max_steps: usize) -> PyResult<()> {
        let mut b = PyBus { obj: bus, err: None };
        let r = self.m.run(&program.p, &mut b, max_steps);
        if let Some(e) = b.err {
            return Err(e);
        }
        r.map_err(sim_error)
    }

    /// Run one instruction: True when it ended the program.
    fn step(&mut self, program: &Program, bus: &Bound<'_, PyAny>) -> PyResult<bool> {
        let mut b = PyBus { obj: bus, err: None };
        let r = self.m.step(&program.p, &mut b);
        if let Some(e) = b.err {
            return Err(e);
        }
        r.map_err(sim_error)
    }
}

#[pymodule]
fn pysgx(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(usse_decode, m)?)?;
    m.add_function(wrap_pyfunction!(usse_encode, m)?)?;
    m.add_function(wrap_pyfunction!(pds_decode, m)?)?;
    m.add_function(wrap_pyfunction!(pds_encode, m)?)?;
    m.add_function(wrap_pyfunction!(assemble, m)?)?;
    m.add_class::<Program>()?;
    m.add_class::<Machine>()?;
    m.add("AsmError", m.py().get_type::<AsmError>())?;
    m.add("SimError", m.py().get_type::<SimError>())?;
    Ok(())
}
