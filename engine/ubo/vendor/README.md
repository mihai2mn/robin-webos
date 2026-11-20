# vendor/rustc-hash

rustc-hash 2.1.3 (MIT OR Apache-2.0, see its LICENSE files) with ONE change: `FxHasher` always uses the 64-bit algorithm
(64-bit state and multiplier, `multiply_mix` through u128, rotate 26), also on 32-bit targets.

Why: adblock-rust's flatbuffer hash tables (generic class/id cosmetic rules) place keys by `FxHasher` of the key. `engine.dat`
is built on a 64-bit host; the TV's 32-bit ARM build of the extension used the 32-bit FxHasher, looked in the wrong slots and
found no generic rule (`wren_ubo_hidden` returned an empty style sheet, nothing was ever hidden by class/id). Wired in through
`[patch.crates-io]` in `../Cargo.toml`. On a 64-bit host the hash is unchanged, so existing engine.dat files stay valid.
