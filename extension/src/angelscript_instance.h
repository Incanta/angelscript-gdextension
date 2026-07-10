#ifndef ANGELSCRIPT_INSTANCE_H
#define ANGELSCRIPT_INSTANCE_H

#include "angelscript_script.h"

#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/object.hpp>

namespace gdas {

// Per-object bridge between Godot and a live asIScriptObject. Not a GDCLASS;
// plugged into Godot through GDExtensionScriptInstanceInfo3.
struct AngelScriptInstance {
	godot::Object *owner = nullptr;
	godot::Ref<AngelScriptScript> script;
	asIScriptObject *object = nullptr; // strong reference

	static godot::HashMap<godot::Object *, AngelScriptInstance *> owner_map;

	static const GDExtensionScriptInstanceInfo3 *get_instance_info();
	static asIScriptObject *lookup_script_object(godot::Object *p_owner);

	// Calls a method on the script object; used by call_func and internally.
	godot::Variant call_method(const godot::StringName &p_method, const godot::Variant **p_args,
			int p_argc, GDExtensionCallError &r_error);

	bool get_property(const godot::StringName &p_name, godot::Variant &r_value);
	bool set_property(const godot::StringName &p_name, const godot::Variant &p_value);

	AngelScriptInstance(godot::Object *p_owner, const godot::Ref<AngelScriptScript> &p_script,
			asIScriptObject *p_object);
	~AngelScriptInstance();
};

} // namespace gdas

#endif // ANGELSCRIPT_INSTANCE_H
