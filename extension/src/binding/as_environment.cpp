#include "as_environment.h"

#include "builtin_types.h"
#include "native_classes.h"

#include "../debugger/as_debugger.h"

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

AsEnvironment *AsEnvironment::singleton = nullptr;

static void message_callback(const asSMessageInfo *p_msg, void *p_param) {
	static_cast<AsEnvironment *>(p_param)->on_message(p_msg);
}

AsEnvironment::AsEnvironment() {
	engine = asCreateScriptEngine();
	engine->SetMessageCallback(asFUNCTION(message_callback), this, asCALL_CDECL);

	// Godot-friendly defaults.
	engine->SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS, 1);
	engine->SetEngineProperty(asEP_ALLOW_IMPLICIT_HANDLE_TYPES, 1);
	// Property accessors require the explicit `property` keyword (mode 3);
	// implicit get_/set_ accessors would shadow real methods like
	// AnimationPlayer.queue via get_queue().
	engine->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, 3);
	engine->SetEngineProperty(asEP_AUTO_GARBAGE_COLLECT, 1);

	register_builtin_types(engine);
	register_native_classes(engine);
}

AsEnvironment::~AsEnvironment() {
	clear_keepalive();
	for (asIScriptContext *ctx : context_pool) {
		ctx->Release();
	}
	context_pool.clear();
	if (engine) {
		engine->ShutDownAndRelease();
		engine = nullptr;
	}
}

AsEnvironment *AsEnvironment::get_singleton() {
	return singleton;
}

void AsEnvironment::create_singleton() {
	if (!singleton) {
		singleton = memnew(AsEnvironment);
	}
}

void AsEnvironment::free_singleton() {
	if (singleton) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

asIScriptModule *AsEnvironment::get_main_module(bool p_create) {
	return engine->GetModule(MAIN_MODULE, p_create ? asGM_CREATE_IF_NOT_EXISTS : asGM_ONLY_IF_EXISTS);
}

static void debugger_line_callback(asIScriptContext *p_ctx, void *p_data) {
	AsDebugger *debugger = AsDebugger::get_singleton();
	if (debugger != nullptr) {
		debugger->on_line(p_ctx);
	}
}

static void debugger_exception_callback(asIScriptContext *p_ctx, void *p_data) {
	AsDebugger *debugger = AsDebugger::get_singleton();
	if (debugger != nullptr) {
		debugger->on_exception(p_ctx);
	}
}

asIScriptContext *AsEnvironment::acquire_context() {
	if (!context_pool.is_empty()) {
		asIScriptContext *ctx = context_pool[context_pool.size() - 1];
		context_pool.remove_at(context_pool.size() - 1);
		return ctx;
	}
	asIScriptContext *ctx = engine->CreateContext();
	if (AsDebugger::get_singleton() != nullptr) {
		ctx->SetLineCallback(asFUNCTION(debugger_line_callback), nullptr, asCALL_CDECL);
		ctx->SetExceptionCallback(asFUNCTION(debugger_exception_callback), nullptr, asCALL_CDECL);
	}
	return ctx;
}

void AsEnvironment::release_context(asIScriptContext *p_context) {
	p_context->Unprepare();
	context_pool.push_back(p_context);
}

void AsEnvironment::keep_alive(RefCounted *p_object) {
	if (p_object == nullptr) {
		return;
	}
	uint64_t id = p_object->get_instance_id();
	if (!keepalive.has(id)) {
		keepalive.insert(id, Ref<RefCounted>(p_object));
	}
}

void AsEnvironment::clear_keepalive() {
	keepalive.clear();
}

void AsEnvironment::begin_message_capture() {
	messages.clear();
	capture_messages = true;
}

LocalVector<AsMessage> AsEnvironment::end_message_capture() {
	capture_messages = false;
	LocalVector<AsMessage> result = messages;
	messages.clear();
	return result;
}

void AsEnvironment::on_message(const asSMessageInfo *p_msg) {
	if (capture_messages) {
		AsMessage m;
		m.section = String::utf8(p_msg->section);
		m.row = p_msg->row;
		m.col = p_msg->col;
		m.type = p_msg->type;
		m.message = String::utf8(p_msg->message);
		messages.push_back(m);
		return;
	}
	String text = vformat("AngelScript: %s(%d,%d): %s", String::utf8(p_msg->section), p_msg->row, p_msg->col, String::utf8(p_msg->message));
	switch (p_msg->type) {
		case asMSGTYPE_ERROR:
			ERR_PRINT(text);
			break;
		case asMSGTYPE_WARNING:
			WARN_PRINT(text);
			break;
		default:
			UtilityFunctions::print_verbose(text);
			break;
	}
}

} // namespace gdas
