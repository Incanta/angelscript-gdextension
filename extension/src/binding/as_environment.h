#ifndef AS_ENVIRONMENT_H
#define AS_ENVIRONMENT_H

#include <angelscript.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace gdas {

// One row of compiler output, captured during builds/validation.
struct AsMessage {
	godot::String section;
	int row = 0;
	int col = 0;
	asEMsgType type = asMSGTYPE_ERROR;
	godot::String message;
};

// Owns the single asIScriptEngine shared by every script, the shared module
// all project scripts compile into, and a pool of reusable contexts.
class AsEnvironment {
	asIScriptEngine *engine = nullptr;
	godot::LocalVector<asIScriptContext *> context_pool;

	// Holds references to RefCounted objects handed to script code, since
	// native classes are registered without refcounting (see ARCHITECTURE.md).
	godot::HashMap<uint64_t, godot::Ref<godot::RefCounted>> keepalive;

	godot::LocalVector<AsMessage> messages;
	bool capture_messages = false;

	static AsEnvironment *singleton;

	AsEnvironment();

public:
	static constexpr const char *MAIN_MODULE = "GodotScripts";

	static AsEnvironment *get_singleton();
	static void create_singleton();
	static void free_singleton();

	asIScriptEngine *get_engine() const { return engine; }
	asIScriptModule *get_main_module(bool p_create = false);

	asIScriptContext *acquire_context();
	void release_context(asIScriptContext *p_context);

	// Stack of contexts currently executing script code (innermost last).
	godot::LocalVector<asIScriptContext *> active_contexts;
	void push_active_context(asIScriptContext *p_ctx) { active_contexts.push_back(p_ctx); }
	void pop_active_context() {
		if (!active_contexts.is_empty()) {
			active_contexts.remove_at(active_contexts.size() - 1);
		}
	}
	asIScriptContext *get_active_context() const {
		return active_contexts.is_empty() ? nullptr : active_contexts[active_contexts.size() - 1];
	}

	void keep_alive(godot::RefCounted *p_object);
	void clear_keepalive();

	// Message capture, used by module builds and _validate.
	void begin_message_capture();
	godot::LocalVector<AsMessage> end_message_capture();
	void on_message(const asSMessageInfo *p_msg);

	~AsEnvironment();
};

} // namespace gdas

#endif // AS_ENVIRONMENT_H
