//! Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf, loginusers.vdf). Keys lower-cased.
use std::collections::BTreeMap;

#[derive(Debug, Clone)]
pub enum Kv {
    Str(String),
    Obj(BTreeMap<String, Kv>),
}

impl Kv {
    pub fn get(&self, k: &str) -> Option<&Kv> {
        match self {
            Kv::Obj(m) => m.get(k),
            _ => None,
        }
    }
    pub fn str(&self, k: &str) -> Option<&str> {
        match self.get(k)? {
            Kv::Str(s) => Some(s),
            _ => None,
        }
    }
    pub fn entries(&self) -> impl Iterator<Item = (&String, &Kv)> {
        let m = match self {
            Kv::Obj(m) => Some(m),
            _ => None,
        };
        m.into_iter().flat_map(|m| m.iter())
    }
}

pub fn parse(text: &str) -> Kv {
    let mut stack: Vec<(Option<String>, BTreeMap<String, Kv>)> = vec![(None, BTreeMap::new())];
    let mut pending: Option<String> = None;
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                let mut s = String::new();
                while let Some(c) = chars.next() {
                    match c {
                        '\\' => {
                            if let Some(n) = chars.next() {
                                s.push(match n {
                                    'n' => '\n',
                                    't' => '\t',
                                    o => o,
                                });
                            }
                        }
                        '"' => break,
                        o => s.push(o),
                    }
                }
                match pending.take() {
                    None => pending = Some(s.to_lowercase()),
                    Some(k) => {
                        stack.last_mut().unwrap().1.insert(k, Kv::Str(s));
                    }
                }
            }
            '{' => stack.push((pending.take(), BTreeMap::new())),
            '}' => {
                if stack.len() > 1 {
                    let (k, m) = stack.pop().unwrap();
                    stack.last_mut().unwrap().1.insert(k.unwrap_or_default(), Kv::Obj(m));
                }
            }
            '/' if chars.peek() == Some(&'/') => {
                for c in chars.by_ref() {
                    if c == '\n' {
                        break;
                    }
                }
            }
            _ => {}
        }
    }
    while stack.len() > 1 {
        let (k, m) = stack.pop().unwrap();
        stack.last_mut().unwrap().1.insert(k.unwrap_or_default(), Kv::Obj(m));
    }
    Kv::Obj(stack.pop().unwrap().1)
}
