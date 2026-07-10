#include "as_debugger.h"

#include "../angelscript_language.h"
#include "../binding/as_environment.h"
#include "../binding/variant_bridge.h"
#include "../lsp/tooling_server.h"

#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/memory.hpp>

using namespace godot;

namespace gdas {

AsDebugger *AsDebugger::singleton = nullptr;

AsDebugger *AsDebugger::get_singleton() {
	return singleton;
}

void AsDebugger::create_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(AsDebugger);
	}
}

void AsDebugger::free_singleton() {
	if (singleton != nullptr) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void AsDebugger::set_breakpoints(const String &p_path, const PackedInt32Array &p_lines) {
	if (p_lines.is_empty()) {
		breakpoints.erase(p_path);
		return;
	}
	HashSet<int> lines;
	for (int line : p_lines) {
		lines.insert(line);
	}
	breakpoints.insert(p_path, lines);
}

static uint32_t context_depth(asIScriptContext *p_ctx) {
	return p_ctx->GetCallstackSize();
}

void AsDebugger::on_line(asIScriptContext *p_ctx) {
	const char *section_cstr = nullptr;
	int line = p_ctx->GetLineNumber(0, nullptr, &section_cstr);
	if (section_cstr == nullptr) {
		return;
	}

	// Feed Godot's built-in debugger.
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	bool godot_debugger_active = engine_debugger != nullptr && engine_debugger->is_active();
	if (godot_debugger_active) {
		if ((poll_counter++ & 0x3F) == 0) {
			engine_debugger->line_poll();
		}
		bool do_break = false;
		int lines_left = engine_debugger->get_lines_left();
		if (lines_left > 0) {
			int depth = engine_debugger->get_depth();
			if (depth < 0 || int(context_depth(p_ctx)) <= depth) {
				engine_debugger->set_lines_left(lines_left - 1);
				if (lines_left - 1 <= 0) {
					do_break = true;
				}
			}
		}
		String section = String::utf8(section_cstr);
		if (!do_break && engine_debugger->is_breakpoint(line, StringName(section)) &&
				!engine_debugger->is_skipping_breakpoints()) {
			do_break = true;
		}
		if (do_break) {
			AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
			engine_debugger->script_debug(lang, true, false);
		}
	}

	// VS Code side.
	bool hit = false;
	String reason;
	if (pause_requested) {
		pause_requested = false;
		hit = true;
		reason = "pause";
	}
	if (!hit && step_mode != STEP_NONE) {
		uint32_t depth = context_depth(p_ctx);
		bool depth_ok = (step_mode == STEP_IN) ||
				(step_mode == STEP_OVER && depth <= step_depth) ||
				(step_mode == STEP_OUT && depth < step_depth);
		if (depth_ok) {
			hit = true;
			reason = "step";
		}
	}
	if (!hit && !breakpoints.is_empty()) {
		String section = String::utf8(section_cstr);
		const HashSet<int> *lines = breakpoints.getptr(section);
		if (lines != nullptr && lines->has(line)) {
			hit = true;
			reason = "breakpoint";
		}
	}
	if (hit) {
		step_mode = STEP_NONE;
		enter_stopped_loop(p_ctx, reason, String());
	}
}

void AsDebugger::on_exception(asIScriptContext *p_ctx) {
	if (ToolingServer::get_singleton() == nullptr || !ToolingServer::get_singleton()->has_debug_client()) {
		return;
	}
	enter_stopped_loop(p_ctx, "exception", String::utf8(p_ctx->GetExceptionString()));
}

void AsDebugger::enter_stopped_loop(asIScriptContext *p_ctx, const String &p_reason, const String &p_text) {
	ToolingServer *server = ToolingServer::get_singleton();
	if (server == nullptr || !server->has_debug_client()) {
		return;
	}
	stopped = true;
	stopped_context = p_ctx;
	resume_action = RESUME_NONE;

	const char *section = nullptr;
	int line = p_ctx->GetLineNumber(0, nullptr, &section);
	Dictionary event;
	event["type"] = "stopped";
	event["reason"] = p_reason;
	event["path"] = section != nullptr ? String::utf8(section) : String();
	event["line"] = line;
	event["text"] = p_text;
	server->broadcast(event);

	// Block the executing thread, servicing the tooling socket until resumed.
	while (resume_action == RESUME_NONE) {
		server->poll();
		if (!server->has_debug_client()) {
			resume_action = RESUME_CONTINUE; // debugger detached
			break;
		}
		OS::get_singleton()->delay_msec(10);
	}

	switch (resume_action) {
		case RESUME_STEP_OVER:
			step_mode = STEP_OVER;
			step_depth = context_depth(p_ctx);
			break;
		case RESUME_STEP_IN:
			step_mode = STEP_IN;
			step_depth = context_depth(p_ctx);
			break;
		case RESUME_STEP_OUT:
			step_mode = STEP_OUT;
			step_depth = context_depth(p_ctx);
			break;
		default:
			step_mode = STEP_NONE;
			break;
	}

	stopped = false;
	stopped_context = nullptr;
	resume_action = RESUME_NONE;

	Dictionary continued;
	continued["type"] = "continued";
	server->broadcast(continued);
}

Array AsDebugger::get_stack_frames() const {
	Array frames;
	asIScriptContext *ctx = stopped_context;
	if (ctx == nullptr) {
		return frames;
	}
	for (asUINT level = 0; level < ctx->GetCallstackSize(); level++) {
		const char *section = nullptr;
		int line = ctx->GetLineNumber(level, nullptr, &section);
		asIScriptFunction *func = ctx->GetFunction(level);
		Dictionary frame;
		frame["id"] = int(level);
		frame["func"] = func != nullptr ? String::utf8(func->GetDeclaration(false, false, true)) : String("?");
		frame["path"] = section != nullptr ? String::utf8(section) : String();
		frame["line"] = line;
		frames.push_back(frame);
	}
	return frames;
}

Array AsDebugger::get_variables(int p_frame, const String &p_scope) const {
	Array out;
	asIScriptContext *ctx = stopped_context;
	if (ctx == nullptr) {
		return out;
	}
	asUINT level = asUINT(p_frame);
	if (p_scope == "members") {
		asIScriptObject *object = static_cast<asIScriptObject *>(ctx->GetThisPointer(level));
		if (object != nullptr) {
			for (asUINT i = 0; i < object->GetPropertyCount(); i++) {
				String name = String::utf8(object->GetPropertyName(i));
				if (name.begins_with("__")) {
					continue;
				}
				int type_id = object->GetPropertyTypeId(i);
				Variant value = as_to_variant(ctx->GetEngine(), object->GetAddressOfProperty(i), type_id);
				Dictionary var;
				var["name"] = name;
				var["value"] = value.stringify();
				var["type"] = String::utf8(ctx->GetEngine()->GetTypeDeclaration(type_id, true));
				out.push_back(var);
			}
		}
		return out;
	}
	for (int i = 0; i < ctx->GetVarCount(level); i++) {
		const char *name = nullptr;
		int type_id = 0;
		if (ctx->GetVar(asUINT(i), level, &name, &type_id) < 0) {
			continue;
		}
		if (name == nullptr || name[0] == '\0' || !ctx->IsVarInScope(asUINT(i), level)) {
			continue;
		}
		Variant value = as_to_variant(ctx->GetEngine(), ctx->GetAddressOfVar(asUINT(i), level), type_id);
		Dictionary var;
		var["name"] = String::utf8(name);
		var["value"] = value.stringify();
		var["type"] = String::utf8(ctx->GetEngine()->GetTypeDeclaration(type_id, true));
		out.push_back(var);
	}
	return out;
}

Dictionary AsDebugger::evaluate(int p_frame, const String &p_expression) const {
	Dictionary out;
	String expr = p_expression.strip_edges();

	// v1: resolve plain local/member names.
	Array locals = get_variables(p_frame, "locals");
	for (int i = 0; i < locals.size(); i++) {
		Dictionary var = locals[i];
		if (String(var["name"]) == expr) {
			out["value"] = var["value"];
			out["value_type"] = var["type"];
			return out;
		}
	}
	Array members = get_variables(p_frame, "members");
	for (int i = 0; i < members.size(); i++) {
		Dictionary var = members[i];
		if (String(var["name"]) == expr) {
			out["value"] = var["value"];
			out["value_type"] = var["type"];
			return out;
		}
	}
	out["value"] = String("<unable to evaluate>");
	out["value_type"] = String();
	return out;
}

} // namespace gdas
