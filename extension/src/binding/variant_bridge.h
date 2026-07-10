#ifndef VARIANT_BRIDGE_H
#define VARIANT_BRIDGE_H

#include <angelscript.h>

#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <utility>

namespace gdas {

// ---------------------------------------------------------------------------
// Builtin type table: maps Godot builtin value types (registered 1:1 as
// AngelScript value types over the godot-cpp C++ classes) to their memory
// operations and Variant conversions.
// ---------------------------------------------------------------------------

// Variant::Type for a Godot/extension_api type name ("Vector2", "int", ...).
// Returns Variant::VARIANT_MAX when the name is not a builtin value type.
godot::Variant::Type builtin_type_from_name(const godot::String &p_name);

size_t builtin_size(godot::Variant::Type p_type);
int builtin_type_traits(godot::Variant::Type p_type); // asGetTypeTraits flags
void builtin_construct(godot::Variant::Type p_type, void *p_mem);
void builtin_copy_construct(godot::Variant::Type p_type, void *p_mem, const void *p_src);
void builtin_destruct(godot::Variant::Type p_type, void *p_mem);
void builtin_assign(godot::Variant::Type p_type, void *p_dst, const void *p_src);
godot::Variant builtin_to_variant(godot::Variant::Type p_type, const void *p_src);
// Constructs *uninitialized* p_dst from a Variant (with conversion).
void builtin_construct_from_variant(const godot::Variant &p_value, godot::Variant::Type p_type, void *p_dst);

// ---------------------------------------------------------------------------
// AngelScript type id <-> Variant::Type registry, filled during registration.
// ---------------------------------------------------------------------------

void bridge_register_value_type(godot::Variant::Type p_type, int p_as_type_id);
int bridge_get_as_type_id(godot::Variant::Type p_type);
// Variant::VARIANT_MAX if the id is not a registered Godot value type.
godot::Variant::Type bridge_get_variant_type(int p_as_type_id);

// User data attached to native class asITypeInfo (namespace godot::).
constexpr asPWORD NATIVE_CLASS_USERDATA = 0x600D0001;
struct NativeClassInfo {
	godot::StringName godot_class;
	bool refcounted = false;
};

// ---------------------------------------------------------------------------
// Generic conversion entry points.
// ---------------------------------------------------------------------------

// Reads a value at p_ref with AngelScript type p_type_id into a Variant.
// Handles primitives, registered value types, Variant, native handles, and
// script objects (unwrapped to their owner Object).
godot::Variant as_to_variant(asIScriptEngine *p_engine, void *p_ref, int p_type_id);

// Argument helpers for asIScriptGeneric thunks.
godot::Variant generic_arg_to_variant(asIScriptGeneric *p_gen, int p_index);
void generic_set_return(asIScriptGeneric *p_gen, const godot::Variant &p_value);

// Argument helpers for asIScriptContext calls (calling script functions).
// Marshals p_value into argument p_index of the prepared context. Temporary
// object arguments are appended to p_temporaries and must outlive Execute().
struct ArgTemporaries {
	godot::LocalVector<godot::Variant> variants;
	godot::LocalVector<std::pair<godot::Variant::Type, void *>> values;
	~ArgTemporaries();
};
bool set_context_arg(asIScriptContext *p_ctx, asIScriptFunction *p_func, asUINT p_index,
		const godot::Variant &p_value, ArgTemporaries &p_temporaries);
godot::Variant get_context_return(asIScriptContext *p_ctx, asIScriptFunction *p_func);

// Writes a Variant into an already-constructed value at p_ref (e.g. a script
// object property). Returns false if the types cannot be converted.
bool variant_to_as_ref(asIScriptEngine *p_engine, const godot::Variant &p_value, void *p_ref, int p_type_id);

// ---------------------------------------------------------------------------
// Object layer hooks (implemented in script_bases.cpp / native_classes.cpp).
// ---------------------------------------------------------------------------

// Converts an Object* to a value suitable for an AS handle of p_target type.
// For native godot:: types this is the Object pointer itself; for script-base
// classes it wraps (or finds) an asIScriptObject. Returned handle is NOT
// addref'ed for native types; script objects ARE addref'ed (caller releases).
void *object_to_as_handle(godot::Object *p_object, asITypeInfo *p_target);

// Extracts the Godot Object* behind an AS handle (native or script object).
godot::Object *as_handle_to_object(void *p_obj, asITypeInfo *p_type);

// ---------------------------------------------------------------------------
// Declaration building (shared by native_classes and script_bases).
// ---------------------------------------------------------------------------

// Maps a Godot type name from reflection to an AngelScript type usable in
// declarations. Object classes become "godot::Name@". Unknown -> "Variant".
godot::String as_type_from_godot_name(const godot::String &p_name);
// Same but for a PropertyInfo-style (type, class_name, hint, usage) tuple.
godot::String as_type_from_property(const godot::Dictionary &p_property_info);
godot::String as_param_decl(const godot::String &p_as_type);
godot::String sanitize_identifier(const godot::String &p_name);

} // namespace gdas

#endif // VARIANT_BRIDGE_H
