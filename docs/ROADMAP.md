# GDScript parity roadmap

Legend: [x] done, [~] partial, [ ] planned.

## Core scripting

- [x] `.as` files load as Script resources (loader/saver, script server)
- [x] Attach scripts to nodes, virtual callbacks (`_ready`, `_process`, ...)
- [x] Exported properties via `[export]` metadata (inspector + placeholders)
- [x] Script signals via `[signal]` metadata
- [x] Method/property/signal reflection (`get_method_list`, etc.)
- [x] Cross-file class references (single shared module)
- [~] Native API coverage: all ClassDB classes via reflection; value types
      hand-registered (core math/containers done, long tail via codegen later)
- [~] Vararg engine methods (overloads up to 8 args)
- [ ] MethodBind fast-path dispatch (currently `Object::callv`)
- [ ] Global class names (`class_name` equivalent) in create-node dialogs
- [ ] `await`/coroutines mapped to `asIScriptContext` suspension
- [ ] RPC config metadata (`[rpc]`)
- [ ] Static methods/variables surface to Godot
- [ ] Bytecode cache for faster startup (GodotBases compiles ~13 s at launch)
- [ ] Proper RefCounted lifetime (currently a keepalive registry pins every
      RefCounted handed to scripts; see ARCHITECTURE.md)
- [ ] Attach script instances when script classes are constructed directly
      (`Player()` creates the native node but does not bind the script yet)
- [ ] Editor-registered classes (bindings are built at SCENE init; classes
      registered later, e.g. editor-only ones, are not mirrored)

## Editor

- [x] Validation with per-line errors (`_validate`)
- [x] Script templates
- [x] Syntax word lists (reserved words, comment/string delimiters)
- [~] Code completion (`_complete_code`: keywords + engine/script members)
- [~] Go-to-definition (`_lookup_code`: script symbols)
- [ ] Editor documentation from doc comments (`_get_documentation`)
- [ ] External editor hand-off
- [ ] Warnings system

## Debugging / profiling

- [x] Breakpoints, stepping, stack, locals in Godot's debugger (`_debug_*`)
- [x] Error backtraces (`_debug_get_current_stack_info`)
- [ ] Expression evaluation in the debugger
- [ ] Profiler (`_profiling_*` with per-function timing)

## VS Code

- [x] Syntax highlighting (TextMate grammar)
- [x] Debug adapter: launch/attach, breakpoints, stepping, stack, variables
- [~] IntelliSense from engine type database (completion, hover)
- [ ] Full workspace parser for offline IntelliSense
- [ ] Semantic tokens, inlay hints, rename, references

## Infrastructure

- [x] Linux build (SCons)
- [~] Windows/macOS/Android/iOS/Web builds (SConstruct supports; CI matrix)
- [x] Headless smoke tests
- [ ] Release packaging (zip addon + .vsix)
