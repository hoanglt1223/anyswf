//! Numbering of every AVM1 action block in a SWF. The asset timelines refer to scripts by
//! this index and the C emitter names its functions after it, so both must use this one
//! traversal (file order, sprites inline, clip actions and button actions in record order).

use std::collections::HashMap;
use swf::Tag;

pub mod kind {
    pub const FRAME: u8 = 1;
    pub const INIT: u8 = 2;
    pub const CLIP_EVENT: u8 = 3;
    pub const BUTTON: u8 = 4;
}

#[derive(Clone, Copy, Debug)]
pub struct Script<'a> {
    pub kind: u8,
    /// Sprite id for frame scripts (0 = root), sprite id for init actions, button id for
    /// button actions, 0 for clip events.
    pub owner: u16,
    pub bytes: &'a [u8],
}

#[derive(Default)]
pub struct Scripts<'a> {
    pub list: Vec<Script<'a>>,
    by_ptr: HashMap<(usize, usize), u32>,
}

impl<'a> Scripts<'a> {
    pub fn collect(tags: &'a [Tag<'a>]) -> Self {
        let mut s = Scripts::default();
        s.walk(tags, 0);
        s
    }

    fn add(&mut self, kind: u8, owner: u16, bytes: &'a [u8]) {
        let key = (bytes.as_ptr() as usize, bytes.len());
        if self.by_ptr.contains_key(&key) {
            return;
        }
        self.by_ptr.insert(key, self.list.len() as u32);
        self.list.push(Script { kind, owner, bytes });
    }

    fn walk(&mut self, tags: &'a [Tag<'a>], owner: u16) {
        for t in tags {
            match t {
                Tag::DoAction(d) => self.add(kind::FRAME, owner, d),
                Tag::DoInitAction { id, action_data } => self.add(kind::INIT, *id, action_data),
                Tag::DefineSprite(sp) => self.walk(&sp.tags, sp.id),
                Tag::PlaceObject(p) => {
                    if let Some(ca) = &p.clip_actions {
                        for r in &ca.records {
                            self.add(kind::CLIP_EVENT, 0, r.action_data);
                        }
                    }
                }
                Tag::DefineButton(b) | Tag::DefineButton2(b) => {
                    for a in &b.actions {
                        self.add(kind::BUTTON, b.id, a.action_data);
                    }
                }
                _ => {}
            }
        }
    }

    /// Index of an action block previously seen by `collect`.
    pub fn index_of(&self, bytes: &[u8]) -> Option<u32> {
        self.by_ptr
            .get(&(bytes.as_ptr() as usize, bytes.len()))
            .copied()
    }
}
