# Architecture

## Overview

Three deliverables in one monorepo:

1. `extension/` — a GDExtension (C++17) embedding the AngelScript runtime and
   implementing Godot's `ScriptLanguageExtension` so `.as` files behave like
   GDScript files.
2. `vscode/` — a VS Code extension: TextMate grammar, language server, and a
   Debug Adapter Protocol implementation. Both talk to the running engine over
   a TCP side channel served by the GDExtension.
3. `demo/` — a Godot project used for manual testing and headless smoke tests.

## GDExtension core (`extension/src/`)

Modeled on lua-gdextension's proven structure:

| Class | Base | Role |
| --- | --- | --- |
| `AngelScriptLanguage` | `ScriptLanguageExtension` | Singleton. Owns the `asIScriptEngine`, validation, completion, templates, debug hooks. |
| `AngelScriptScript` | `ScriptExtension` | One per `.as` resource. Holds source, compiled class metadata (methods, properties, signals, exports). |
| `AngelScriptInstance` | plain struct | Per-object bridge. Implements `GDExtensionScriptInstanceInfo3` (created with `script_instance_create3`, attached with `object_set_script_instance`). Wraps an `asIScriptObject`. |
| `AngelScriptResourceFormatLoader/Saver` | `ResourceFormatLoader/Saver` | Recognize/load/save `.as` files as `AngelScriptScript`. |

Registration happens at `MODULE_INITIALIZATION_LEVEL_SCENE` in
`register_types.cpp`; the language is registered with
`Engine::register_script_language`.

### Type system bridge

AngelScript is statically typed and its script classes cannot inherit from
application-registered (C++) types. To still support the GDScript-like model
(`class Player : CharacterBody2D`), the bridge has two layers, both driven by
runtime reflection over Godot's `ClassDB` singleton (so it never goes stale
against the engine version):

1. **Native layer, namespace `godot::`.** Every ClassDB class is registered
   with the AngelScript engine as a reference type (`asOBJ_REF`); RefCounted
   subtree uses real add/release, raw Objects use `asOBJ_NOCOUNT`. Methods,
   property accessors, and constants are registered with generic thunks that
   convert AngelScript args to `Variant` and dispatch through `Object::callv`
   (correctness first; MethodBind fast-path is a planned optimization).
   Vararg engine methods get overloads taking 0..N `Variant` parameters.

2. **Script-space layer, global namespace.** For engine classes, thin script
   base classes are generated as AngelScript source and compiled into the
   shared module: `shared abstract class CharacterBody2D : Node2D { ... }`
   rooted at `GodotObject`, each holding a typed native handle `__self` and
   forwarding every method/property to the native layer. User scripts inherit
   these, so engine members resolve statically and without an `owner.` prefix.
   Objects returned from engine calls are wrapped by a factory that first
   checks whether the object already has a live AngelScript instance (identity
   is preserved: you get the actual `Player`, not a fresh wrapper).

Value types (`Vector2`, `Color`, `Array`, `Dictionary`, `String`,
`StringName`, `NodePath`, `Callable`, `Signal`, `Variant`, ...) are registered
by hand as AngelScript value types over the godot-cpp C++ classes with
`asCALL_THISCALL` (fast, no conversion). `Variant` is a first-class registered
type with implicit conversions, used for vararg calls and duck-typed APIs.

### Script model

- One shared `asIScriptEngine`; all project scripts compile into a single
  module so cross-file class references work like GDScript. Any `_reload`
  schedules a (debounced) module rebuild.
- `CScriptBuilder` (from the AngelScript SDK add-ons) preprocesses sources and
  collects metadata:
  - `[export]`, `[export_range(0, 10)]`, ... on class fields → exported
    properties (inspector, placeholders).
  - `[signal]` on an empty method declaration → script signal.
  - `[tool]` on the class → tool script.
- The "main" class of a file is the class whose name matches the file name
  (PascalCase), else the first class extending a Godot base.
- Godot virtual callbacks (`_ready`, `_process`, ...) are ordinary methods on
  the script class, found by name at `call` time via `asITypeInfo`.

### Instance lifecycle

`AngelScriptScript::_instance_create` → create `asIScriptObject` (factory via
`asIScriptContext`), set `__self` to the owner, build the
`GDExtensionScriptInstanceInfo3` table:

- `call_func` → look up `asIScriptFunction` by name on the type, execute on a
  pooled context, convert args/return through the Variant bridge.
- `get_func`/`set_func` → script class fields via `asIScriptObject`
  property table; falls back to `_get`/`_set` if defined, then native owner.
- `get_property_list_func`/`get_method_list_func` → from cached class
  metadata.
- Placeholders (editor) use `placeholder_script_instance_create/update`.

### Debug + tooling channel

`AngelScriptLanguage` implements the `_debug_*` group (line/function/stack,
locals via `asIScriptContext::GetVarCount/GetAddressOfVar`), which is enough
for Godot's built-in debugger UI and, transitively, Godot's own DAP.

Additionally, in debug builds the extension runs a small TCP server
(default port `27500`, project setting `angelscript/lsp/port`):

- Streams a JSON **type database** (native classes + script classes) to the
  VS Code language server for IntelliSense (same pattern as
  Hazelight's Unreal AngelScript extension).
- Serves a line-oriented JSON debug protocol (breakpoints, step, stack,
  variables) implemented over `asIScriptContext` line callbacks, consumed by
  the VS Code debug adapter.

Message framing: one JSON object per line (`\n`-terminated, UTF-8), each with
a `"type"` field. See `docs/PROTOCOL.md`.

## VS Code extension (`vscode/`)

- TextMate grammar adapted from Hazelight's `source.angelscript` (Unreal
  macros removed, Godot annotations added).
- Language server (`vscode-languageserver`): document sync, diagnostics,
  completion/hover/definition backed by the engine type database when
  connected, with a lightweight fallback parser when offline.
- Debug adapter (`@vscode/debugadapter`): DAP ↔ engine TCP protocol.

## Build

SCons. `extension/SConstruct` clones godot-cpp's build environment (same
technique as lua-gdextension), builds the AngelScript SDK sources
(`extension/angelscript/sdk/angelscript/source`) plus the needed add-ons, and
links everything into `addons/godot-angelscript/bin/` per platform/arch.
`compatibility_minimum` is Godot 4.5.
