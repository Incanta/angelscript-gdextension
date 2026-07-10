#ifndef SCRIPT_BASES_H
#define SCRIPT_BASES_H

#include <angelscript.h>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace gdas {

// Registers the `ref` handle type (scripthandle add-on) and the __wrap /
// __take_pending helpers used by generated code.
void register_wrap_helpers(asIScriptEngine *p_engine);

// Generates AngelScript source mirroring every ClassDB class as a `shared`
// script class (GodotObject -> Object -> Node -> ...) and compiles it into
// the persistent "GodotBases" module. Returns true on success.
bool build_bases_module(asIScriptEngine *p_engine);

// Header text injected into user modules: `external shared class X;` for each
// generated base plus per-module singleton property accessors (Input, ...).
godot::String get_user_module_header();
// Number of lines the header occupies (for error line remapping).
int get_user_module_header_line_count();

// Finds the generated script-base asITypeInfo for a Godot class, walking up
// the inheritance chain if the exact class was not generated.
asITypeInfo *find_script_base_type(const godot::StringName &p_class);

// Wraps a Godot object into a script object: returns the live script instance
// object if the object runs one of our scripts, otherwise creates a plain
// script-base wrapper. Returned pointer carries a strong reference.
asIScriptObject *wrap_object(godot::Object *p_object);

// Set by AngelScriptInstance to expose live script instances to wrap_object.
using InstanceLookupFn = asIScriptObject *(*)(godot::Object *);
void set_instance_lookup(InstanceLookupFn p_fn);

// Pending-owner slot consumed by the generated GodotObject constructor.
void set_pending_owner(godot::Object *p_object);
// While true, constructing script objects does not create native objects
// (used when probing default property values).
void set_suppress_native_instantiate(bool p_suppress);

void script_bases_clear_cache();

} // namespace gdas

#endif // SCRIPT_BASES_H
