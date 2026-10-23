//! Control flow and static stack depths for one decoded code block.
//!
//! When every reachable instruction has a single known stack depth (and every variable
//! argument count is a pushed constant), the emitter maps stack slots to fixed C
//! array indices. Otherwise (for-in loops, computed argument counts, inconsistent
//! merges, underflow) the block uses a runtime stack pointer.
//!
//! Region rules follow Ruffle: a With/Try/Catch/Finally body behaves like a child
//! activation, so control leaving it by a jump ends the body (execution continues at
//! the body's exit, not at the jump target).

use super::decode::{Code, Instr, RegionKind};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use swf::avm1::types::{Action, Value};

/// Where control goes when an instruction finishes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Point {
    /// Start of the instruction at this address.
    At(usize),
    /// Exit glue of region `i` (pops scopes / leaves try handlers, then continues).
    Exit(usize),
    /// End of the code block: implicit return.
    End,
}

#[derive(Default, Debug)]
pub struct Flow {
    pub dynamic: bool,
    pub reason: Option<String>,
    /// Static depth on entry to each reachable point.
    pub depth: BTreeMap<Point, i32>,
    pub max_depth: i32,
    pub reachable: BTreeSet<usize>,
    /// Points that need a C label.
    pub labels: BTreeSet<Point>,
}

/// Innermost region containing `addr`.
pub fn innermost(code: &Code, addr: usize) -> Option<usize> {
    code.regions
        .iter()
        .enumerate()
        .filter(|(_, r)| r.start <= addr && addr < r.end)
        .min_by_key(|(_, r)| r.end - r.start)
        .map(|(i, _)| i)
}

/// Where execution continues after region `i` is left.
pub fn region_exit_target(code: &Code, i: usize) -> Point {
    let r = &code.regions[i];
    let siblings: Vec<usize> = code
        .regions
        .iter()
        .enumerate()
        .filter(|(_, s)| s.owner == r.owner)
        .map(|(j, _)| j)
        .collect();
    let finally = siblings
        .iter()
        .copied()
        .find(|&j| code.regions[j].kind == RegionKind::Finally);
    let after = siblings
        .iter()
        .map(|&j| code.regions[j].end)
        .max()
        .unwrap_or(r.end);
    match r.kind {
        RegionKind::With | RegionKind::Finally => resolve_addr(code, r.owner, after),
        RegionKind::TryBody | RegionKind::Catch => match finally {
            Some(f) => Point::At(code.regions[f].start),
            None => resolve_addr(code, r.owner, after),
        },
    }
}

/// A target address seen from `from`: jumps leaving the innermost region become that
/// region's exit.
pub fn resolve_addr(code: &Code, from: usize, target: usize) -> Point {
    if let Some(ri) = innermost(code, from) {
        let r = &code.regions[ri];
        if target < r.start || target >= r.end {
            return Point::Exit(ri);
        }
    }
    if target >= code.len {
        Point::End
    } else {
        Point::At(target)
    }
}

/// Successors of an instruction (normal control flow only).
pub fn successors(code: &Code, ins: &Instr) -> Vec<Point> {
    let fall = resolve_addr(code, ins.addr, ins.next);
    match &ins.action {
        Action::Jump(j) => vec![jump(code, ins, j.offset)],
        Action::If(j) => vec![fall, jump(code, ins, j.offset)],
        Action::Return | Action::Throw | Action::End => vec![],
        Action::WaitForFrame(w) => vec![fall, skip_actions(code, ins, w.num_actions_to_skip)],
        Action::WaitForFrame2(w) => vec![fall, skip_actions(code, ins, w.num_actions_to_skip)],
        Action::Try(_) => vec![Point::At(ins.next).min_end(code)],
        // `with (null)` skips its body.
        Action::With(_) => {
            let ri = code
                .regions
                .iter()
                .position(|r| r.owner == ins.addr && r.kind == RegionKind::With)
                .expect("decoder records every with");
            vec![
                Point::At(ins.next).min_end(code),
                region_exit_target(code, ri),
            ]
        }
        _ => vec![fall],
    }
}

trait MinEnd {
    fn min_end(self, code: &Code) -> Point;
}

impl MinEnd for Point {
    fn min_end(self, code: &Code) -> Point {
        match self {
            Point::At(a) if a >= code.len => Point::End,
            p => p,
        }
    }
}

fn jump(code: &Code, ins: &Instr, offset: i16) -> Point {
    let t = ins.next as i64 + offset as i64;
    if t < 0 || t as usize >= code.len {
        // Jumping outside the block ends it (Ruffle treats it as an implicit return).
        if let Some(ri) = innermost(code, ins.addr) {
            return Point::Exit(ri);
        }
        return Point::End;
    }
    resolve_addr(code, ins.addr, t as usize)
}

/// WaitForFrame skips a number of *actions*, not bytes.
pub fn skip_actions(code: &Code, ins: &Instr, n: u8) -> Point {
    let Some(i) = code.index_of(ins.addr) else {
        return Point::End;
    };
    match code.instrs.get(i + 1 + n as usize) {
        Some(t) => resolve_addr(code, ins.addr, t.addr),
        None => Point::End,
    }
}

/// Stack effect: (pops, pushes), or None when it depends on runtime values.
fn effect(ins: &Instr, consts: &[Option<f64>]) -> Option<(i32, i32)> {
    let c = |i: usize| {
        consts
            .get(consts.len().wrapping_sub(1 + i))
            .copied()
            .flatten()
    };
    let count = |i: usize| {
        c(i).filter(|v| *v >= 0.0 && *v <= 65535.0 && v.fract() == 0.0)
            .map(|v| v as i32)
    };
    Some(match &ins.action {
        Action::Push(p) => (0, p.values.len() as i32),
        Action::Pop => (1, 0),
        Action::PushDuplicate => (1, 2),
        Action::StackSwap => (2, 2),
        Action::Add
        | Action::Add2
        | Action::Subtract
        | Action::Multiply
        | Action::Divide
        | Action::Modulo
        | Action::Equals
        | Action::Equals2
        | Action::StrictEquals
        | Action::Less
        | Action::Less2
        | Action::Greater
        | Action::And
        | Action::Or
        | Action::BitAnd
        | Action::BitOr
        | Action::BitXor
        | Action::BitLShift
        | Action::BitRShift
        | Action::BitURShift
        | Action::StringAdd
        | Action::StringEquals
        | Action::StringLess
        | Action::StringGreater
        | Action::InstanceOf
        | Action::CastOp
        | Action::GetMember
        | Action::Delete
        | Action::GetProperty => (2, 1),
        Action::Not
        | Action::Increment
        | Action::Decrement
        | Action::ToInteger
        | Action::ToNumber
        | Action::ToString
        | Action::TypeOf
        | Action::StringLength
        | Action::MBStringLength
        | Action::AsciiToChar
        | Action::CharToAscii
        | Action::MBAsciiToChar
        | Action::MBCharToAscii
        | Action::GetVariable
        | Action::RandomNumber
        | Action::TargetPath
        | Action::Delete2 => (1, 1),
        Action::StringExtract | Action::MBStringExtract => (3, 1),
        Action::SetVariable | Action::DefineLocal | Action::Extends | Action::GetUrl2(_) => (2, 0),
        Action::SetMember | Action::SetProperty | Action::CloneSprite => (3, 0),
        Action::DefineLocal2
        | Action::RemoveSprite
        | Action::Trace
        | Action::Throw
        | Action::Return
        | Action::If(_)
        | Action::SetTarget2
        | Action::GotoFrame2(_)
        | Action::Call
        | Action::WaitForFrame2(_)
        | Action::With(_) => (1, 0),
        Action::GetTime => (0, 1),
        Action::StoreRegister(_) => (1, 1),
        Action::DefineFunction(f) => (0, if f.name.is_empty() { 1 } else { 0 }),
        Action::DefineFunction2(f) => (0, if f.name.is_empty() { 1 } else { 0 }),
        Action::CallFunction | Action::NewObject => (2 + count(1)?, 1),
        Action::CallMethod | Action::NewMethod => (3 + count(2)?, 1),
        Action::InitArray => (1 + count(0)?, 1),
        Action::InitObject => (1 + 2 * count(0)?, 1),
        Action::ImplementsOp => (2 + count(1)?, 0),
        Action::StartDrag => {
            let constrain = c(2)?;
            (if constrain == 1.0 { 7 } else { 3 }, 0)
        }
        Action::Enumerate | Action::Enumerate2 => return None,
        _ => (0, 0),
    })
}

fn push_consts(ins: &Instr, consts: &mut Vec<Option<f64>>, pops: i32, pushes: i32) {
    match &ins.action {
        Action::Push(p) => {
            for v in &p.values {
                consts.push(match v {
                    Value::Int(i) => Some(*i as f64),
                    Value::Double(d) => Some(*d),
                    Value::Float(f) => Some(*f as f64),
                    Value::Bool(b) => Some(*b as i32 as f64),
                    _ => None,
                });
            }
        }
        Action::PushDuplicate => {
            let top = consts.last().copied().flatten();
            consts.push(top);
        }
        Action::StackSwap => {
            let n = consts.len();
            if n >= 2 {
                consts.swap(n - 1, n - 2);
            }
        }
        Action::StoreRegister(_) => {}
        _ => {
            for _ in 0..pops {
                consts.pop();
            }
            for _ in 0..pushes {
                consts.push(None);
            }
        }
    }
}

fn entry_depth(try_addr: usize, flow: &Flow) -> Option<i32> {
    flow.depth.get(&Point::At(try_addr)).copied()
}

pub fn analyze(code: &Code) -> Flow {
    let mut flow = Flow::default();
    let mut work: VecDeque<(Point, i32, Vec<Option<f64>>)> = VecDeque::new();
    work.push_back((Point::At(0).min_end(code), 0, Vec::new()));
    let fail = |flow: &mut Flow, why: String| {
        if !flow.dynamic {
            flow.dynamic = true;
            flow.reason = Some(why);
        }
    };
    // Entry points reached only through exceptions.
    let mut pending_regions: Vec<(Point, usize)> = Vec::new();
    loop {
        let Some((pt, depth, consts)) = work.pop_front() else {
            // Catch and finally bodies are entered through exceptions, at the depth the
            // Try saw; walk them once the normal flow is exhausted.
            if pending_regions.is_empty() {
                break;
            }
            for (p, ri) in std::mem::take(&mut pending_regions) {
                let d = entry_depth(code.regions[ri].owner, &flow).unwrap_or(0);
                work.push_back((p, d, Vec::new()));
            }
            continue;
        };
        if let Some(&d) = flow.depth.get(&pt) {
            if d != depth {
                fail(
                    &mut flow,
                    format!("stack depth differs at {pt:?} ({d} vs {depth})"),
                );
            }
            continue;
        }
        flow.depth.insert(pt, depth);
        flow.max_depth = flow.max_depth.max(depth);
        let addr = match pt {
            Point::At(a) => a,
            Point::Exit(ri) => {
                let next = region_exit_target(code, ri);
                flow.labels.insert(pt);
                flow.labels.insert(next);
                work.push_back((next, depth, Vec::new()));
                continue;
            }
            Point::End => continue,
        };
        let Some(idx) = code.index_of(addr) else {
            fail(
                &mut flow,
                format!("jump into the middle of an action at {addr}"),
            );
            continue;
        };
        let ins = &code.instrs[idx];
        flow.reachable.insert(addr);
        let Some((pops, pushes)) = effect(ins, &consts) else {
            fail(&mut flow, format!("variable stack effect at {addr}"));
            // Keep walking so reachability and labels stay complete.
            let succ = successors(code, ins);
            let branches = succ.len() > 1 || matches!(ins.action, Action::Jump(_));
            for s in succ {
                if branches || matches!(s, Point::Exit(_)) {
                    flow.labels.insert(s);
                }
                if !flow.depth.contains_key(&s) {
                    work.push_back((s, depth, Vec::new()));
                }
            }
            continue;
        };
        if pops > depth {
            fail(&mut flow, format!("stack underflow at {addr}"));
        }
        let after = depth - pops.min(depth) + pushes;
        flow.max_depth = flow.max_depth.max(after);
        let mut nc = consts.clone();
        push_consts(ins, &mut nc, pops, pushes);
        if let Action::Try(_) = ins.action {
            for (ri, r) in code.regions.iter().enumerate() {
                if r.owner == addr && matches!(r.kind, RegionKind::Catch | RegionKind::Finally) {
                    pending_regions.push((Point::At(r.start).min_end(code), ri));
                    flow.labels.insert(Point::At(r.start).min_end(code));
                }
            }
        }
        let succ = successors(code, ins);
        let branches = succ.len() > 1 || matches!(ins.action, Action::Jump(_));
        for s in succ {
            if branches || matches!(s, Point::Exit(_)) {
                flow.labels.insert(s);
            }
            work.push_back((s, after, if branches { Vec::new() } else { nc.clone() }));
        }
    }
    flow
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::avm1::decode::decode;

    #[test]
    fn straight_line_is_static() {
        // Push 1, 2; Add2; Trace
        let b = [
            0x96, 0x0A, 0x00, 0x07, 1, 0, 0, 0, 0x07, 2, 0, 0, 0, 0x47, 0x26,
        ];
        let code = decode(&b, 8).unwrap();
        let f = analyze(&code);
        assert!(!f.dynamic, "{:?}", f.reason);
        assert_eq!(f.max_depth, 2);
        assert_eq!(f.depth[&Point::End], 0);
    }

    #[test]
    fn constant_call_count_is_static_and_enumerate_is_dynamic() {
        // Push 7, 1, "f"; CallFunction; Pop
        let b = [
            0x96, 0x0D, 0x00, 0x07, 7, 0, 0, 0, 0x07, 1, 0, 0, 0, 0x00, b'f', 0x00, 0x3D, 0x17,
        ];
        let f = analyze(&decode(&b, 8).unwrap());
        assert!(!f.dynamic, "{:?}", f.reason);
        assert_eq!(f.max_depth, 3);
        // Push "o"; Enumerate2
        let b = [0x96, 0x03, 0x00, 0x00, b'o', 0x00, 0x55];
        assert!(analyze(&decode(&b, 8).unwrap()).dynamic);
    }

    #[test]
    fn loop_back_edge_keeps_depth() {
        // L: Push true; If L  (If jumps back by its own length + push length)
        let b = [0x96, 0x02, 0x00, 0x05, 0x01, 0x9D, 0x02, 0x00, 0xF6, 0xFF];
        let f = analyze(&decode(&b, 8).unwrap());
        assert!(!f.dynamic, "{:?}", f.reason);
        assert!(f.labels.contains(&Point::At(0)));
    }
}
