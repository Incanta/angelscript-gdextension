#ifndef BUILTIN_TYPES_H
#define BUILTIN_TYPES_H

#include <angelscript.h>

namespace gdas {

// Phase 1: registers the value types themselves (Vector2, Array, String with
// its string factory, Variant, ...) plus lifecycle behaviors and global enums.
// Must run before native class types are registered.
void register_builtin_types(asIScriptEngine *p_engine);

// Phase 2: registers methods, operators, constructors, member accessors, and
// utility functions from the generated extension_api tables. Runs after
// native class *types* exist because some signatures reference godot::Object.
void register_builtin_members(asIScriptEngine *p_engine);

} // namespace gdas

#endif // BUILTIN_TYPES_H
