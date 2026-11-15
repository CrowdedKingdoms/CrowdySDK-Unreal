# Wasmtime for Mod Scripts

The Mod Scripts runtime links Wasmtime's C API as a static library, built to run WebAssembly through its Pulley
interpreter only: Cranelift is built in to turn a module into Pulley bytecode, and no native code is ever generated.

- Version: Wasmtime 50.0.1, tag `v50.0.1`, commit `c307ec81de02852d815e9849f7c89d955c03c30d`.
- Licence: Apache-2.0 WITH LLVM-exception (`LICENSE`). Every crate linked into `wasmtime.lib` and its licence is in
  `THIRD-PARTY-NOTICES.txt`; games that ship the CrowdySDK include that file in their notices.
- Target: `x86_64-pc-windows-msvc`, dynamic CRT (`/MD`), Rust 1.98.1.

## Rebuild

```
git clone --depth 1 --branch v50.0.1 https://github.com/bytecodealliance/wasmtime.git src
cmake -S src/crates/c-api -B build -DWASMTIME_DISABLE_ALL_FEATURES=ON -DWASMTIME_FEATURE_CRANELIFT=ON -DWASMTIME_FEATURE_PULLEY=ON -DWASMTIME_FEATURE_DISABLE_LOGGING=ON -DCMAKE_INSTALL_PREFIX=install
cmake --build build --config Release
cmake --install build --config Release
```

Copy `install/include` to `include/` and `install/lib/wasmtime.lib` to `lib/Win64/`.

## Licence check

From `src`, with `cargo-deny` 0.20.2 and `cargo-about` 0.9.3 (`--features cli`):

```
cargo deny --manifest-path crates/c-api/artifact/Cargo.toml --config ../crowdy-deny.toml check licenses
cargo about generate --manifest-path crates/c-api/artifact/Cargo.toml -c ../about.toml --no-default-features --features "cranelift pulley disable-logging" -o ../THIRD-PARTY-NOTICES.txt ../about.hbs
```

`crowdy-deny.toml` allows only permissive licences; a GPL, LGPL, AGPL or MPL crate fails the check.
