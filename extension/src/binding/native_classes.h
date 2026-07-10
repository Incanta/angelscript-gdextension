#ifndef NATIVE_CLASSES_H
#define NATIVE_CLASSES_H

#include <angelscript.h>

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace gdas {

// Reflected info about one ClassDB class, used both for native registration
// and script-base source generation.
struct ClassSpec {
	godot::StringName name;
	godot::StringName parent;
	bool refcounted = false;
	bool instantiable = false;
	godot::TypedArray<godot::Dictionary> methods; // own methods (no inheritance)
	godot::TypedArray<godot::Dictionary> properties;
	godot::TypedArray<godot::Dictionary> signals;
};

// Registers every ClassDB class as a reference type in namespace `godot`,
// including methods, implicit up/down casts, enums/constants, factories,
// and helper globals (__wrap, __singleton, __instantiate, __signal).
// Also invokes register_builtin_members() at the right phase.
void register_native_classes(asIScriptEngine *p_engine);
void native_classes_free_bind_data();

// Reflection cache produced during registration (name -> spec).
const godot::HashMap<godot::StringName, ClassSpec> &get_class_specs();
bool is_native_class(const godot::StringName &p_name);

// Best-effort AngelScript literal for a default argument value ("" = cannot
// encode). p_param_type is the AS parameter type (handles end with '@').
godot::String variant_to_as_literal(const godot::Variant &p_value, const godot::String &p_param_type);

} // namespace gdas

#endif // NATIVE_CLASSES_H
