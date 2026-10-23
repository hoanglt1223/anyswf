//! Build-time interning: every string the generated code names (identifiers, literals,
//! constant pool entries) becomes an index into one atom table, interned once at
//! startup by the runtime.

use std::collections::HashMap;

#[derive(Default)]
pub struct Atoms {
    pub list: Vec<String>,
    index: HashMap<String, u32>,
}

impl Atoms {
    pub fn intern(&mut self, s: &str) -> u32 {
        if let Some(&i) = self.index.get(s) {
            return i;
        }
        let i = self.list.len() as u32;
        self.list.push(s.to_owned());
        self.index.insert(s.to_owned(), i);
        i
    }
}

/// A C string literal for arbitrary UTF-8 (octal escapes keep it ASCII-clean; `?` is
/// escaped so no trigraph can form).
pub fn c_literal(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for b in s.bytes() {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'?' => out.push_str("\\?"),
            0x20..=0x7E => out.push(b as char),
            _ => out.push_str(&format!("\\{b:03o}")),
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn interning_and_escaping() {
        let mut a = Atoms::default();
        assert_eq!(a.intern("x"), 0);
        assert_eq!(a.intern("y"), 1);
        assert_eq!(a.intern("x"), 0);
        assert_eq!(
            c_literal("a\"b\\c\n\u{e9}??"),
            "\"a\\\"b\\\\c\\012\\303\\251\\?\\?\""
        );
    }
}
