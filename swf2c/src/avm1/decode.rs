//! Decodes an AVM1 action block into instructions addressed by byte offset.
//!
//! `With` and `Try` bodies are decoded inline (they share the parent's address space,
//! so jumps can be resolved), and recorded as regions. `DefineFunction` bodies are
//! separate code blocks, compiled as separate C functions.

use crate::{Error, Result};
use swf::avm1::types::{Action, CatchVar, DefineFunction2, FunctionFlags};

#[derive(Clone, Debug)]
pub struct Instr<'a> {
    pub addr: usize,
    pub next: usize,
    pub action: Action<'a>,
    /// For DefineFunction(2): index into `Code::functions`.
    pub func: Option<usize>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegionKind {
    With,
    TryBody,
    Catch,
    Finally,
}

/// A `With` or `Try` sub-block: [start, end) in the parent's address space.
#[derive(Clone, Debug)]
pub struct Region<'a> {
    pub kind: RegionKind,
    pub start: usize,
    pub end: usize,
    /// Address of the With/Try instruction that opened the region.
    pub owner: usize,
    /// Try only: the catch variable.
    pub catch_var: Option<CatchVar<'a>>,
}

#[derive(Clone, Debug)]
pub struct FuncDef<'a> {
    pub def: DefineFunction2<'a>,
    pub df2: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Code<'a> {
    pub instrs: Vec<Instr<'a>>,
    pub regions: Vec<Region<'a>>,
    pub functions: Vec<FuncDef<'a>>,
    pub len: usize,
}

impl<'a> Code<'a> {
    pub fn index_of(&self, addr: usize) -> Option<usize> {
        self.instrs.binary_search_by_key(&addr, |i| i.addr).ok()
    }
}

/// Size of the action record header plus payload (excluding inline sub-blocks).
fn record_len(buf: &[u8], a: usize) -> Option<usize> {
    let op = *buf.get(a)?;
    if op < 0x80 {
        Some(1)
    } else {
        let lo = *buf.get(a + 1)? as usize;
        let hi = *buf.get(a + 2)? as usize;
        Some(3 + (lo | (hi << 8)))
    }
}

pub fn decode<'a>(buf: &'a [u8], version: u8) -> Result<Code<'a>> {
    let mut code = Code {
        len: buf.len(),
        ..Default::default()
    };
    let mut a = 0usize;
    while a < buf.len() {
        let Some(rec) = record_len(buf, a) else {
            break;
        };
        if a + rec > buf.len() {
            // Truncated trailing action: Flash stops executing there.
            break;
        }
        let mut reader = swf::avm1::read::Reader::new(&buf[a..], version);
        let action = match reader.read_action() {
            Ok(action) => action,
            Err(e) => {
                return Err(Error::new(format!("bad AVM1 action at offset {a}: {e}")));
            }
        };
        let mut next = a + rec;
        let mut func = None;
        match &action {
            Action::DefineFunction(f) => {
                next += f.actions.len();
                func = Some(code.functions.len());
                code.functions.push(FuncDef {
                    def: f.clone().into(),
                    df2: false,
                });
            }
            Action::DefineFunction2(f) => {
                next += f.actions.len();
                func = Some(code.functions.len());
                code.functions.push(FuncDef {
                    def: f.clone(),
                    df2: true,
                });
            }
            Action::With(w) => {
                code.regions.push(Region {
                    kind: RegionKind::With,
                    start: next,
                    end: next + w.actions.len(),
                    owner: a,
                    catch_var: None,
                });
            }
            Action::Try(t) => {
                let ts = next;
                let te = ts + t.try_body.len();
                code.regions.push(Region {
                    kind: RegionKind::TryBody,
                    start: ts,
                    end: te,
                    owner: a,
                    catch_var: t.catch_body.as_ref().map(|(v, _)| v.clone()),
                });
                let mut e = te;
                if let Some((_, body)) = &t.catch_body {
                    code.regions.push(Region {
                        kind: RegionKind::Catch,
                        start: e,
                        end: e + body.len(),
                        owner: a,
                        catch_var: None,
                    });
                    e += body.len();
                }
                if let Some(body) = t.finally_body {
                    code.regions.push(Region {
                        kind: RegionKind::Finally,
                        start: e,
                        end: e + body.len(),
                        owner: a,
                        catch_var: None,
                    });
                }
            }
            _ => {}
        }
        code.instrs.push(Instr {
            addr: a,
            next,
            action,
            func,
        });
        a = next;
    }
    for r in &code.regions {
        if r.end > buf.len() {
            return Err(Error::new(format!(
                "AVM1 {:?} block at offset {} runs past the end of its code",
                r.kind, r.owner
            )));
        }
    }
    Ok(code)
}

/// Absolute jump target of a Jump/If at `instr`.
pub fn jump_target(instr: &Instr, offset: i16) -> i64 {
    instr.next as i64 + offset as i64
}

pub fn flags_of(f: &FuncDef) -> FunctionFlags {
    f.def.flags
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_push_trace_and_function_bodies() {
        // Push "a"; Trace; DefineFunction "f"() { Push 1; Return }; End
        let mut b = vec![0x96, 0x03, 0x00, 0x00, b'a', 0x00, 0x26];
        b.extend_from_slice(&[0x9B, 0x06, 0x00, b'f', 0x00, 0x00, 0x00, 0x09, 0x00]);
        b.extend_from_slice(&[0x96, 0x05, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x3E]);
        b.push(0x00);
        let code = decode(&b, 8).unwrap();
        assert_eq!(code.instrs.len(), 4);
        assert_eq!(code.instrs[2].func, Some(0));
        assert_eq!(code.instrs[3].addr, b.len() - 1);
        assert!(matches!(code.instrs[3].action, Action::End));
        let body = code.functions[0].def.actions;
        let inner = decode(body, 8).unwrap();
        assert_eq!(inner.instrs.len(), 2);
    }

    #[test]
    fn with_body_is_inline_region() {
        // Push "o"; With(2) { Pop }; Push 1
        let b = vec![
            0x96, 0x03, 0x00, 0x00, b'o', 0x00, 0x94, 0x02, 0x00, 0x01, 0x00, 0x17, 0x96, 0x02,
            0x00, 0x05, 0x01,
        ];
        let code = decode(&b, 8).unwrap();
        assert_eq!(code.regions.len(), 1);
        assert_eq!((code.regions[0].start, code.regions[0].end), (11, 12));
        assert_eq!(code.instrs.len(), 4);
        assert!(matches!(code.instrs[2].action, Action::Pop));
    }
}
