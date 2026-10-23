//! AVM1 bytecode to C. Each script and each DefineFunction body becomes one C function
//! of type `swf_code_fn`; control flow becomes labels and gotos; the operand stack
//! becomes an array with fixed indices when its depth is static (see stack_analysis).
//! All semantics live in the runtime (runtime/include/swfrt/avm1.h).

use super::decode::{Code, Instr, RegionKind, decode};
use super::stack_analysis::{Flow, Point, analyze, region_exit_target, resolve_addr, skip_actions};
use super::strings::{Atoms, c_literal};
use crate::Result;
use crate::scripts::Scripts;
use std::collections::HashMap;
use std::fmt::Write as _;
use swf::avm1::types::{Action, CatchVar, Value};

/// Extra stack slots for blocks with a runtime stack pointer (for-in pushes all keys).
const DYNAMIC_STACK: i32 = 4096;

pub struct Output {
    pub source: String,
    pub functions: usize,
    pub dynamic_blocks: usize,
    pub warnings: Vec<String>,
}

struct Emitter {
    version: u8,
    atoms: Atoms,
    pools: Vec<Vec<u32>>,
    pool_index: HashMap<Vec<u32>, usize>,
    protos: String,
    descs: String,
    bodies: String,
    nfuncs: usize,
    dynamic_blocks: usize,
    warnings: Vec<String>,
}

fn decode_str(s: &swf::SwfStr, version: u8) -> String {
    s.to_string_lossy(swf::SwfStr::encoding_for_version(version))
}

fn c_double(v: f64) -> String {
    if v.is_nan() || v.is_infinite() {
        // Constant 0.0/0.0 is a compile error in MSVC; build the value from its bits.
        format!("swf_f64_bits(0x{:016X}ULL)", v.to_bits())
    } else {
        let s = format!("{v:?}");
        if s.contains('.') || s.contains('e') {
            s
        } else {
            format!("{s}.0")
        }
    }
}

/// One code block being emitted.
struct Block<'c, 'a> {
    code: &'c Code<'a>,
    flow: Flow,
    out: String,
    /// Try owner address -> handler number.
    tries: HashMap<usize, usize>,
}

impl Block<'_, '_> {
    fn label(&self, p: Point) -> String {
        match p {
            Point::At(a) => format!("L{a}"),
            Point::Exit(r) => format!("X{r}"),
            Point::End => "L_END".into(),
        }
    }
    fn dynamic(&self) -> bool {
        self.flow.dynamic
    }
}

impl Emitter {
    fn atom(&mut self, s: &str) -> u32 {
        self.atoms.intern(s)
    }

    fn pool(&mut self, names: Vec<u32>) -> usize {
        if let Some(&i) = self.pool_index.get(&names) {
            return i;
        }
        let i = self.pools.len();
        self.pool_index.insert(names.clone(), i);
        self.pools.push(names);
        i
    }

    fn push_expr(&mut self, v: &Value) -> String {
        match v {
            Value::Undefined => "swf_undef()".into(),
            Value::Null => "swf_null()".into(),
            Value::Bool(b) => format!("swf_bool({})", *b as i32),
            Value::Int(i) => format!("swf_num({i}.0)"),
            Value::Float(f) => format!("swf_num({})", c_double(*f as f64)),
            Value::Double(d) => format!("swf_num({})", c_double(*d)),
            Value::Str(s) => {
                let a = self.atom(&decode_str(s, self.version));
                format!("swf_strv(ATOMS[{a}])")
            }
            Value::Register(r) => format!("swf_reg_get(cx, {r})"),
            Value::ConstantPool(i) => format!("swf_pool_get(cx, {i})"),
        }
    }

    /// Compiles a code block into a C function named `name`.
    fn block(&mut self, name: &str, bytes: &[u8]) -> Result<()> {
        let code = decode(bytes, self.version)?;
        let flow = analyze(&code);
        if flow.dynamic {
            self.dynamic_blocks += 1;
        }
        let mut b = Block {
            code: &code,
            flow,
            out: String::new(),
            tries: HashMap::new(),
        };
        let has_try = code
            .instrs
            .iter()
            .any(|i| matches!(i.action, Action::Try(_)));
        for ins in &code.instrs {
            if matches!(ins.action, Action::Try(_)) && b.flow.reachable.contains(&ins.addr) {
                let n = b.tries.len();
                b.tries.insert(ins.addr, n);
            }
        }
        let cap = if b.dynamic() {
            b.flow.max_depth.max(0) + DYNAMIC_STACK
        } else {
            b.flow.max_depth.max(1)
        };
        let mut head = String::new();
        let _ = writeln!(head, "static swf_val {name}(swf_ctx *cx)\n{{");
        let _ = writeln!(
            head,
            "    swf_val *S = swf_frame_alloc(cx, {cap});\n    (void)S;"
        );
        if b.dynamic() {
            let vol = if has_try { "volatile " } else { "" };
            let _ = writeln!(head, "    {vol}int sp = 0;");
            let _ = writeln!(head, "    (void)sp;");
        }
        for (ri, r) in code.regions.iter().enumerate() {
            if r.kind == RegionKind::With && b.flow.reachable.contains(&r.owner) {
                let _ = writeln!(head, "    swf_scope *volatile W{ri} = NULL;");
            }
        }
        for n in 0..b.tries.len() {
            let _ = writeln!(
                head,
                "    swf_try *H{n} = (swf_try *)swf_frame_alloc(cx, (sizeof(swf_try) + sizeof(swf_val) - 1) / sizeof(swf_val));"
            );
            let _ = writeln!(
                head,
                "    swf_try *HC{n} = (swf_try *)swf_frame_alloc(cx, (sizeof(swf_try) + sizeof(swf_val) - 1) / sizeof(swf_val));"
            );
            let _ = writeln!(head, "    swf_val *E{n} = swf_frame_alloc(cx, 2);");
            let _ = writeln!(head, "    volatile int P{n} = 0, R{n} = 0;");
            let _ = writeln!(head, "    (void)HC{n};");
        }

        for (idx, ins) in code.instrs.iter().enumerate() {
            if !b.flow.reachable.contains(&ins.addr) {
                continue;
            }
            if b.flow.labels.contains(&Point::At(ins.addr)) {
                let _ = writeln!(b.out, "L{}:;", ins.addr);
            }
            let d = b.flow.depth.get(&Point::At(ins.addr)).copied().unwrap_or(0);
            let falls = self.instr(&mut b, ins, d)?;
            if falls {
                let next = resolve_addr(&code, ins.addr, ins.next);
                let next = match next {
                    Point::At(a) if a >= code.len => Point::End,
                    p => p,
                };
                let following = code.instrs[idx + 1..]
                    .iter()
                    .find(|i| b.flow.reachable.contains(&i.addr))
                    .map(|i| Point::At(i.addr));
                if Some(next) != following {
                    let l = b.label(next);
                    let _ = writeln!(b.out, "    goto {l};");
                }
            }
        }
        let _ = writeln!(b.out, "L_END:;\n    return swf_undef();");
        self.region_glue(&mut b);
        let _ = writeln!(b.out, "}}\n");
        let _ = writeln!(self.protos, "static swf_val {name}(swf_ctx *cx);");
        self.bodies.push_str(&head);
        self.bodies.push_str(&b.out);
        Ok(())
    }

    /// Exit glue for With/Try/Catch/Finally regions that are entered.
    fn region_glue(&mut self, b: &mut Block) {
        let code = b.code;
        for (ri, r) in code.regions.iter().enumerate() {
            if !b.flow.depth.contains_key(&Point::Exit(ri)) {
                continue;
            }
            let _ = writeln!(b.out, "X{ri}:;");
            let n = b.tries.get(&r.owner).copied();
            let has_finally = code
                .regions
                .iter()
                .any(|s| s.owner == r.owner && s.kind == RegionKind::Finally);
            match (r.kind, n) {
                (RegionKind::With, _) => {
                    let _ = writeln!(b.out, "    cx->scope = W{ri};");
                }
                (RegionKind::TryBody, Some(n)) => {
                    let _ = writeln!(b.out, "    swf_try_leave(cx, H{n});");
                }
                (RegionKind::Catch, Some(n)) if has_finally => {
                    let _ = writeln!(b.out, "    swf_try_leave(cx, HC{n});");
                }
                (RegionKind::Finally, Some(n)) => {
                    let _ = writeln!(
                        b.out,
                        "    if (P{n}) {{ P{n} = 0; swf_throw(cx, E{n}[0]); }}"
                    );
                    let _ = writeln!(b.out, "    if (R{n}) return E{n}[1];");
                }
                _ => {}
            }
            let next = region_exit_target(code, ri);
            let l = b.label(next);
            let _ = writeln!(b.out, "    goto {l};");
        }
    }

    /// Emits a fixed-arity operation. `f` receives the operands, deepest first.
    fn op(
        &mut self,
        b: &mut Block,
        d: i32,
        pops: i32,
        f: impl FnOnce(&[String]) -> Option<String>,
    ) {
        if b.dynamic() {
            let names: Vec<String> = (0..pops).map(|i| format!("o{i}")).collect();
            let _ = write!(b.out, "    {{");
            for i in (0..pops).rev() {
                let _ = write!(b.out, " swf_val o{i} = sp > 0 ? S[--sp] : swf_undef();");
            }
            match f(&names) {
                Some(e) => {
                    let _ = writeln!(
                        b.out,
                        " swf_val r = {e}; if (sp >= {cap}) swf_stack_overflow(cx); S[sp++] = r; }}",
                        cap = b.flow.max_depth.max(0) + DYNAMIC_STACK
                    );
                }
                None => {
                    let _ = writeln!(b.out, " }}");
                }
            }
        } else {
            let base = d - pops;
            let names: Vec<String> = (0..pops).map(|i| format!("S[{}]", base + i)).collect();
            if let Some(e) = f(&names) {
                let _ = writeln!(b.out, "    S[{base}] = {e};");
            }
        }
    }

    /// Same as `op` but for statements with no result.
    fn stmt(&mut self, b: &mut Block, d: i32, pops: i32, f: impl FnOnce(&[String]) -> String) {
        if b.dynamic() {
            let _ = write!(b.out, "    {{");
            for i in (0..pops).rev() {
                let _ = write!(b.out, " swf_val o{i} = sp > 0 ? S[--sp] : swf_undef();");
            }
            let names: Vec<String> = (0..pops).map(|i| format!("o{i}")).collect();
            let _ = writeln!(b.out, " {}; }}", f(&names));
        } else {
            let base = d - pops;
            let names: Vec<String> = (0..pops).map(|i| format!("S[{}]", base + i)).collect();
            let _ = writeln!(b.out, "    {};", f(&names));
        }
    }

    /// Variable-arity actions go through the runtime's stack-pointer helpers.
    fn dyn_call(&mut self, b: &mut Block, d: i32, helper: &str) {
        if b.dynamic() {
            let _ = writeln!(b.out, "    {helper}(cx, S, (int *)&sp);");
        } else {
            let _ = writeln!(b.out, "    {{ int t = {d}; {helper}(cx, S, &t); }}");
        }
    }

    fn jump_to(&self, b: &Block, p: Point) -> String {
        format!("goto {}", b.label(p))
    }

    fn after_call_check(&self, b: &mut Block) {
        let _ = writeln!(b.out, "    if (swf_base_removed(cx)) return swf_undef();");
    }

    /// The innermost Try (with a finally) whose body or catch contains `addr`.
    fn enclosing_finally(&self, code: &Code, addr: usize) -> Option<(usize, usize)> {
        code.regions
            .iter()
            .filter(|r| {
                matches!(r.kind, RegionKind::TryBody | RegionKind::Catch)
                    && r.start <= addr
                    && addr < r.end
            })
            .min_by_key(|r| r.end - r.start)
            .and_then(|r| {
                code.regions
                    .iter()
                    .find(|f| f.owner == r.owner && f.kind == RegionKind::Finally)
                    .map(|f| (r.owner, f.start))
            })
    }

    /// Emits one instruction; returns whether control falls through.
    fn instr(&mut self, b: &mut Block, ins: &Instr, d: i32) -> Result<bool> {
        let code = b.code;
        let v = self.version;
        macro_rules! bin {
            ($f:expr) => {{
                self.op(b, d, 2, |a| Some(format!("{}(cx, {}, {})", $f, a[0], a[1])));
            }};
        }
        macro_rules! un {
            ($f:expr) => {{
                self.op(b, d, 1, |a| Some(format!("{}(cx, {})", $f, a[0])));
            }};
        }
        match &ins.action {
            Action::Push(p) => {
                let exprs: Vec<String> = p.values.iter().map(|x| self.push_expr(x)).collect();
                for (i, e) in exprs.iter().enumerate() {
                    if b.dynamic() {
                        let cap = b.flow.max_depth.max(0) + DYNAMIC_STACK;
                        let _ = writeln!(
                            b.out,
                            "    {{ swf_val r = {e}; if (sp >= {cap}) swf_stack_overflow(cx); S[sp++] = r; }}"
                        );
                    } else {
                        let _ = writeln!(b.out, "    S[{}] = {e};", d + i as i32);
                    }
                }
            }
            Action::Pop => self.stmt(b, d, 1, |_| "(void)0".into()),
            Action::PushDuplicate => {
                if b.dynamic() {
                    let cap = b.flow.max_depth.max(0) + DYNAMIC_STACK;
                    let _ = writeln!(
                        b.out,
                        "    {{ swf_val r = sp > 0 ? S[sp - 1] : swf_undef(); if (sp >= {cap}) swf_stack_overflow(cx); S[sp++] = r; }}"
                    );
                } else {
                    let _ = writeln!(b.out, "    S[{d}] = S[{}];", d - 1);
                }
            }
            Action::StackSwap => {
                if b.dynamic() {
                    let _ = writeln!(
                        b.out,
                        "    {{ swf_val x = sp > 0 ? S[--sp] : swf_undef(); swf_val y = sp > 0 ? S[--sp] : swf_undef(); S[sp++] = x; S[sp++] = y; }}"
                    );
                } else {
                    let _ = writeln!(
                        b.out,
                        "    {{ swf_val t = S[{a}]; S[{a}] = S[{c}]; S[{c}] = t; }}",
                        a = d - 1,
                        c = d - 2
                    );
                }
            }
            Action::StoreRegister(r) => {
                if b.dynamic() {
                    let _ = writeln!(
                        b.out,
                        "    swf_reg_set(cx, {}, sp > 0 ? S[sp - 1] : swf_undef());",
                        r.register
                    );
                } else {
                    let _ = writeln!(b.out, "    swf_reg_set(cx, {}, S[{}]);", r.register, d - 1);
                }
            }
            Action::ConstantPool(cp) => {
                let names: Vec<u32> = cp
                    .strings
                    .iter()
                    .map(|s| self.atom(&decode_str(s, v)))
                    .collect();
                let p = self.pool(names);
                let _ = writeln!(b.out, "    swf_set_pool(cx, &POOL{p});");
            }
            Action::Add => bin!("swf_op_add"),
            Action::Add2 => bin!("swf_op_add2"),
            Action::Subtract => bin!("swf_op_sub"),
            Action::Multiply => bin!("swf_op_mul"),
            Action::Divide => bin!("swf_op_div"),
            Action::Modulo => bin!("swf_op_mod"),
            Action::Equals => bin!("swf_op_equals"),
            Action::Equals2 => bin!("swf_op_equals2"),
            Action::StrictEquals => bin!("swf_op_strict_equals"),
            Action::Less => bin!("swf_op_less"),
            Action::Less2 => bin!("swf_op_less2"),
            Action::Greater => bin!("swf_op_greater"),
            Action::And => bin!("swf_op_and"),
            Action::Or => bin!("swf_op_or"),
            Action::BitAnd => bin!("swf_op_bitand"),
            Action::BitOr => bin!("swf_op_bitor"),
            Action::BitXor => bin!("swf_op_bitxor"),
            Action::BitLShift => bin!("swf_op_lshift"),
            Action::BitRShift => bin!("swf_op_rshift"),
            Action::BitURShift => bin!("swf_op_urshift"),
            Action::StringAdd => bin!("swf_op_string_add"),
            Action::StringEquals => bin!("swf_op_string_equals"),
            Action::StringLess => bin!("swf_op_string_less"),
            Action::StringGreater => bin!("swf_op_string_greater"),
            Action::InstanceOf => bin!("swf_op_instanceof"),
            Action::CastOp => bin!("swf_op_cast"),
            Action::GetMember => bin!("swf_get_member"),
            Action::Delete => bin!("swf_delete"),
            Action::GetProperty => bin!("swf_get_property"),
            Action::Not => un!("swf_op_not"),
            Action::Increment => un!("swf_op_inc"),
            Action::Decrement => un!("swf_op_dec"),
            Action::ToInteger => un!("swf_op_to_integer"),
            Action::ToNumber => un!("swf_op_to_number"),
            Action::ToString => un!("swf_op_to_string"),
            Action::TypeOf => un!("swf_op_typeof"),
            Action::StringLength => un!("swf_op_string_length"),
            Action::MBStringLength => un!("swf_op_mb_string_length"),
            Action::AsciiToChar => un!("swf_op_ascii_to_char"),
            Action::CharToAscii => un!("swf_op_char_to_ascii"),
            Action::MBAsciiToChar => un!("swf_op_mb_ascii_to_char"),
            Action::MBCharToAscii => un!("swf_op_mb_char_to_ascii"),
            Action::GetVariable => un!("swf_get_variable"),
            Action::RandomNumber => un!("swf_op_random"),
            Action::TargetPath => un!("swf_act_target_path"),
            Action::Delete2 => un!("swf_delete2"),
            Action::StringExtract => self.op(b, d, 3, |a| {
                Some(format!(
                    "swf_op_string_extract(cx, {}, {}, {})",
                    a[0], a[1], a[2]
                ))
            }),
            Action::MBStringExtract => self.op(b, d, 3, |a| {
                Some(format!(
                    "swf_op_mb_string_extract(cx, {}, {}, {})",
                    a[0], a[1], a[2]
                ))
            }),
            Action::GetTime => self.op(b, d, 0, |_| Some("swf_op_get_time(cx)".into())),
            Action::SetVariable => self.stmt(b, d, 2, |a| {
                format!("swf_set_variable(cx, {}, {})", a[0], a[1])
            }),
            Action::DefineLocal => self.stmt(b, d, 2, |a| {
                format!("swf_define_local(cx, {}, {})", a[0], a[1])
            }),
            Action::DefineLocal2 => {
                self.stmt(b, d, 1, |a| format!("swf_define_local2(cx, {})", a[0]))
            }
            Action::SetMember => self.stmt(b, d, 3, |a| {
                format!("swf_set_member(cx, {}, {}, {})", a[0], a[1], a[2])
            }),
            Action::SetProperty => self.stmt(b, d, 3, |a| {
                format!("swf_set_property(cx, {}, {}, {})", a[0], a[1], a[2])
            }),
            Action::CloneSprite => self.stmt(b, d, 3, |a| {
                format!("swf_act_clone_sprite(cx, {}, {}, {})", a[0], a[1], a[2])
            }),
            Action::Extends => {
                self.stmt(b, d, 2, |a| format!("swf_extends(cx, {}, {})", a[0], a[1]))
            }
            Action::Trace => self.stmt(b, d, 1, |a| format!("swf_op_trace(cx, {})", a[0])),
            Action::RemoveSprite => {
                self.stmt(b, d, 1, |a| format!("swf_act_remove_sprite(cx, {})", a[0]))
            }
            Action::SetTarget2 => {
                self.stmt(b, d, 1, |a| format!("swf_act_set_target2(cx, {})", a[0]))
            }
            Action::Call => {
                self.stmt(b, d, 1, |a| format!("swf_act_call(cx, {})", a[0]));
                self.after_call_check(b);
            }
            Action::GotoFrame2(g) => self.stmt(b, d, 1, |a| {
                format!(
                    "swf_act_goto_frame2(cx, {}, {}, {})",
                    a[0], g.set_playing as i32, g.scene_offset
                )
            }),
            Action::GetUrl2(g) => {
                let flags = swf_geturl2_flags(g);
                self.stmt(b, d, 2, |a| {
                    format!("swf_act_get_url2(cx, {}, {}, {flags})", a[0], a[1])
                })
            }
            Action::GotoFrame(g) => {
                let _ = writeln!(b.out, "    swf_act_goto_frame(cx, {});", g.frame);
            }
            Action::GotoLabel(l) => {
                let a = self.atom(&decode_str(l.label, v));
                let _ = writeln!(b.out, "    swf_act_goto_label(cx, {a});");
            }
            Action::SetTarget(t) => {
                let a = self.atom(&decode_str(t.target, v));
                let _ = writeln!(b.out, "    swf_act_set_target(cx, {a});");
            }
            Action::GetUrl(g) => {
                let u = self.atom(&decode_str(g.url, v));
                let t = self.atom(&decode_str(g.target, v));
                let _ = writeln!(b.out, "    swf_act_get_url(cx, {u}, {t});");
            }
            Action::NextFrame => {
                let _ = writeln!(b.out, "    swf_act_next_frame(cx);");
            }
            Action::PreviousFrame => {
                let _ = writeln!(b.out, "    swf_act_prev_frame(cx);");
            }
            Action::Play => {
                let _ = writeln!(b.out, "    swf_act_play(cx);");
            }
            Action::Stop => {
                let _ = writeln!(b.out, "    swf_act_stop(cx);");
            }
            Action::StopSounds => {
                let _ = writeln!(b.out, "    swf_act_stop_sounds(cx);");
            }
            Action::ToggleQuality => {
                let _ = writeln!(b.out, "    swf_act_toggle_quality(cx);");
            }
            Action::EndDrag => {
                let _ = writeln!(b.out, "    swf_act_end_drag(cx);");
            }
            Action::StartDrag => self.dyn_call(b, d, "swf_dyn_start_drag"),
            Action::CallFunction => {
                self.dyn_call(b, d, "swf_dyn_call_function");
                self.after_call_check(b);
            }
            Action::CallMethod => {
                self.dyn_call(b, d, "swf_dyn_call_method");
                self.after_call_check(b);
            }
            Action::NewObject => {
                self.dyn_call(b, d, "swf_dyn_new_object");
                self.after_call_check(b);
            }
            Action::NewMethod => {
                self.dyn_call(b, d, "swf_dyn_new_method");
                self.after_call_check(b);
            }
            Action::InitArray => self.dyn_call(b, d, "swf_dyn_init_array"),
            Action::InitObject => self.dyn_call(b, d, "swf_dyn_init_object"),
            Action::ImplementsOp => self.dyn_call(b, d, "swf_dyn_implements"),
            Action::Enumerate | Action::Enumerate2 => {
                let by_name = matches!(ins.action, Action::Enumerate);
                let cap = b.flow.max_depth.max(0) + DYNAMIC_STACK;
                let _ = writeln!(
                    b.out,
                    "    swf_dyn_enumerate(cx, S, (int *)&sp, {cap}, {});",
                    by_name as i32
                );
            }
            Action::Jump(_) => {
                let succ = super::stack_analysis::successors(code, ins);
                let l = self.jump_to(b, succ[0]);
                let _ = writeln!(b.out, "    {l};");
                return Ok(false);
            }
            Action::If(_) => {
                let succ = super::stack_analysis::successors(code, ins);
                let l = self.jump_to(b, succ[1]);
                self.stmt(b, d, 1, |a| format!("if (swf_to_bool(cx, {})) {l}", a[0]));
            }
            Action::WaitForFrame(w) => {
                let skip = skip_actions(code, ins, w.num_actions_to_skip);
                let l = self.jump_to(b, skip);
                let _ = writeln!(
                    b.out,
                    "    if (!swf_act_frame_loaded(cx, {})) {l};",
                    w.frame
                );
            }
            Action::WaitForFrame2(w) => {
                let skip = skip_actions(code, ins, w.num_actions_to_skip);
                let l = self.jump_to(b, skip);
                self.stmt(b, d, 1, |a| {
                    format!("if (!swf_act_frame_loaded2(cx, {})) {l}", a[0])
                });
            }
            Action::Return => {
                if let Some((owner, fin)) = self.enclosing_finally(code, ins.addr) {
                    let n = b.tries[&owner];
                    let l = b.label(Point::At(fin));
                    self.stmt(b, d, 1, |a| {
                        format!("E{n}[1] = {}; R{n} = 1; goto {l}", a[0])
                    });
                } else {
                    self.stmt(b, d, 1, |a| format!("return {}", a[0]));
                }
                return Ok(false);
            }
            Action::Throw => {
                self.stmt(b, d, 1, |a| format!("swf_throw(cx, {})", a[0]));
                return Ok(false);
            }
            Action::End => {
                let _ = writeln!(b.out, "    return swf_undef();");
                return Ok(false);
            }
            Action::DefineFunction(_) | Action::DefineFunction2(_) => {
                let fi = ins.func.expect("decoder records every function");
                let id = self.nfuncs;
                self.nfuncs += 1;
                let f = code.functions[fi].clone();
                let fname = format!("F{id}");
                self.block(&fname, f.def.actions)?;
                let name = decode_str(f.def.name, v);
                let name_atom = if name.is_empty() {
                    -1
                } else {
                    self.atom(&name) as i64
                };
                let mut params = String::new();
                for p in &f.def.params {
                    let a = self.atom(&decode_str(p.name, v));
                    let r = p.register_index.map_or(0, |r| r.get());
                    let _ = write!(params, "{{{a}, {r}}}, ");
                }
                if params.is_empty() {
                    params.push_str("{0, 0}");
                }
                let _ = writeln!(self.descs, "static const swf_param P{id}[] = {{{params}}};");
                let _ = writeln!(
                    self.descs,
                    "static const swf_fn_desc D{id} = {{{fname}, {name_atom}, {}, {}, {}, {}, P{id}}};",
                    f.def.flags.bits(),
                    if f.df2 { f.def.register_count } else { 0 },
                    f.def.params.len(),
                    f.df2 as i32
                );
                if name.is_empty() {
                    self.op(b, d, 0, |_| {
                        Some(format!("swf_define_function(cx, &D{id})"))
                    });
                } else {
                    let _ = writeln!(b.out, "    (void)swf_define_function(cx, &D{id});");
                }
            }
            Action::With(_) => {
                let ri = code
                    .regions
                    .iter()
                    .position(|r| r.owner == ins.addr && r.kind == RegionKind::With)
                    .expect("decoder records every with");
                let after = region_exit_target(code, ri);
                let l = b.label(after);
                let _ = writeln!(b.out, "    W{ri} = cx->scope;");
                self.stmt(b, d, 1, |a| {
                    format!("if (!swf_with_enter(cx, {})) goto {l}", a[0])
                });
            }
            Action::Try(t) => {
                let n = b.tries[&ins.addr];
                let sp = if b.dynamic() {
                    "sp".to_string()
                } else {
                    d.to_string()
                };
                let dynamic = b.dynamic();
                let restore = |h: &str| {
                    if dynamic {
                        format!(" sp = {h}->sp;")
                    } else {
                        String::new()
                    }
                };
                let regions: Vec<(usize, RegionKind, usize)> = code
                    .regions
                    .iter()
                    .enumerate()
                    .filter(|(_, r)| r.owner == ins.addr)
                    .map(|(i, r)| (i, r.kind, r.start))
                    .collect();
                let catch = regions.iter().find(|r| r.1 == RegionKind::Catch);
                let finally = regions.iter().find(|r| r.1 == RegionKind::Finally);
                let after_all = regions
                    .iter()
                    .map(|r| code.regions[r.0].end)
                    .max()
                    .unwrap_or(ins.next);
                let after = resolve_addr(code, ins.addr, after_all);
                let after = match after {
                    Point::At(a) if a >= code.len => Point::End,
                    p => p,
                };
                let _ = writeln!(b.out, "    swf_try_enter(cx, H{n}, {sp});");
                let _ = writeln!(b.out, "    if (setjmp(H{n}->jb)) {{");
                let _ = writeln!(
                    b.out,
                    "        cx->scope = H{n}->scope;{}",
                    restore(&format!("H{n}"))
                );
                match (catch, finally) {
                    (Some(c), _) => {
                        match &t.catch_body {
                            Some((CatchVar::Register(r), _)) => {
                                let _ = writeln!(b.out, "        swf_reg_set(cx, {r}, H{n}->exc);");
                            }
                            Some((CatchVar::Var(name), _)) => {
                                let a = self.atom(&decode_str(name, v));
                                let _ =
                                    writeln!(b.out, "        swf_catch_var(cx, {a}, H{n}->exc);");
                            }
                            None => {}
                        }
                        if let Some(f) = finally {
                            let lf = b.label(Point::At(f.2));
                            let _ = writeln!(b.out, "        swf_try_enter(cx, HC{n}, {sp});");
                            let _ = writeln!(
                                b.out,
                                "        if (setjmp(HC{n}->jb)) {{ cx->scope = HC{n}->scope;{} E{n}[0] = HC{n}->exc; P{n} = 1; goto {lf}; }}",
                                restore(&format!("HC{n}"))
                            );
                        }
                        let lc = b.label(Point::At(c.2));
                        let _ = writeln!(b.out, "        goto {lc};");
                    }
                    (None, Some(f)) => {
                        let lf = b.label(Point::At(f.2));
                        let _ =
                            writeln!(b.out, "        E{n}[0] = H{n}->exc; P{n} = 1; goto {lf};");
                    }
                    (None, None) => {
                        let la = b.label(after);
                        let _ = writeln!(b.out, "        goto {la};");
                    }
                }
                let _ = writeln!(b.out, "    }}");
            }
            Action::Unknown(u) => {
                self.warnings
                    .push(format!("unknown AVM1 opcode 0x{:02X}", u.opcode));
                let _ = writeln!(b.out, "    swf_act_unknown(cx, {});", u.opcode);
            }
        }
        Ok(true)
    }
}

fn swf_geturl2_flags(g: &swf::avm1::types::GetUrl2) -> u8 {
    let mut f = g.send_vars_method() as u8;
    if g.is_target_sprite() {
        f |= 1 << 6;
    }
    if g.is_load_vars() {
        f |= 1 << 7;
    }
    f
}

pub fn emit_movie(swf_version: u8, scripts: &Scripts, source_name: &str) -> Result<Output> {
    let mut e = Emitter {
        version: swf_version,
        atoms: Atoms::default(),
        pools: Vec::new(),
        pool_index: HashMap::new(),
        protos: String::new(),
        descs: String::new(),
        bodies: String::new(),
        nfuncs: 0,
        dynamic_blocks: 0,
        warnings: Vec::new(),
    };
    for (i, s) in scripts.list.iter().enumerate() {
        e.block(&format!("S{i}"), s.bytes)?;
    }
    let mut c = String::new();
    let _ = writeln!(
        c,
        "/* Generated by swf2c {} from {}. Do not edit. */\n#include \"swfrt/avm1.h\"\n\n\
         /* Labels are emitted generously; unused ones are expected. */\n\
         #if defined(_MSC_VER)\n#pragma warning(disable : 4102)\n\
         #elif defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n#endif\n",
        env!("CARGO_PKG_VERSION"),
        source_name.replace("*/", "* /")
    );
    let n = e.atoms.list.len();
    let _ = writeln!(c, "static swf_str *ATOMS[{}];", n.max(1));
    let _ = writeln!(c, "static const char *const ATOM_UTF8[{}] = {{", n.max(1));
    for s in &e.atoms.list {
        let _ = writeln!(c, "    {},", c_literal(s));
    }
    if n == 0 {
        let _ = writeln!(c, "    \"\",");
    }
    let _ = writeln!(
        c,
        "}};\nstatic const uint32_t ATOM_BYTES[{}] = {{",
        n.max(1)
    );
    for s in &e.atoms.list {
        let _ = writeln!(c, "    {},", s.len());
    }
    if n == 0 {
        let _ = writeln!(c, "    0,");
    }
    let _ = writeln!(c, "}};\n");
    for (i, p) in e.pools.iter().enumerate() {
        let list: Vec<String> = p.iter().map(|a| a.to_string()).collect();
        let list = if list.is_empty() {
            "0".to_string()
        } else {
            list.join(", ")
        };
        let _ = writeln!(c, "static const uint32_t POOL{i}_ATOMS[] = {{{list}}};");
        let _ = writeln!(
            c,
            "static const swf_pool POOL{i} = {{{}, POOL{i}_ATOMS}};",
            p.len()
        );
    }
    c.push('\n');
    c.push_str(&e.protos);
    c.push('\n');
    c.push_str(&e.descs);
    c.push('\n');
    c.push_str(&e.bodies);
    let ns = scripts.list.len();
    let _ = writeln!(c, "static const swf_code_fn SCRIPTS[{}] = {{", ns.max(1));
    for i in 0..ns {
        let _ = writeln!(c, "    S{i},");
    }
    if ns == 0 {
        let _ = writeln!(c, "    0,");
    }
    let _ = writeln!(
        c,
        "}};\n\nconst swf_game_code SWF_GAME_CODE = {{SWF_CODE_ABI, {swf_version}, {n}, ATOM_UTF8, ATOM_BYTES, ATOMS, {ns}, SCRIPTS}};"
    );
    Ok(Output {
        source: c,
        functions: e.nfuncs,
        dynamic_blocks: e.dynamic_blocks,
        warnings: e.warnings,
    })
}
