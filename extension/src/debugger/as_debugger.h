#ifndef AS_DEBUGGER_H
#define AS_DEBUGGER_H

#include <angelscript.h>

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace gdas {

class ToolingServer;

// Line-callback driven debugger. Serves both Godot's built-in debugger (via
// the EngineDebugger singleton) and the VS Code debug adapter (via the
// ToolingServer TCP protocol).
class AsDebugger {
public:
	enum StepMode {
		STEP_NONE,
		STEP_OVER,
		STEP_IN,
		STEP_OUT,
	};

private:
	static AsDebugger *singleton;

	// VS Code breakpoints: res:// path -> set of 1-based lines.
	godot::HashMap<godot::String, godot::HashSet<int>> breakpoints;

	StepMode step_mode = STEP_NONE;
	uint32_t step_depth = 0;
	bool pause_requested = false;

	// While stopped, the blocked thread spins on this; commands from the
	// tooling server mutate it.
	enum ResumeAction {
		RESUME_NONE,
		RESUME_CONTINUE,
		RESUME_STEP_OVER,
		RESUME_STEP_IN,
		RESUME_STEP_OUT,
	};
	ResumeAction resume_action = RESUME_NONE;
	bool stopped = false;
	asIScriptContext *stopped_context = nullptr;

	uint32_t poll_counter = 0;

	void enter_stopped_loop(asIScriptContext *p_ctx, const godot::String &p_reason,
			const godot::String &p_text);

public:
	static AsDebugger *get_singleton();
	static void create_singleton();
	static void free_singleton();

	// Installed on every context via asIScriptContext::SetLineCallback.
	void on_line(asIScriptContext *p_ctx);
	void on_exception(asIScriptContext *p_ctx);

	void set_breakpoints(const godot::String &p_path, const godot::PackedInt32Array &p_lines);
	void request_pause() { pause_requested = true; }
	void resume(ResumeAction p_action) { resume_action = p_action; }
	bool is_stopped() const { return stopped; }
	asIScriptContext *get_stopped_context() const { return stopped_context; }

	// Called by the tooling server while the game thread is blocked.
	godot::Array get_stack_frames() const;
	godot::Array get_variables(int p_frame, const godot::String &p_scope) const;
	godot::Dictionary evaluate(int p_frame, const godot::String &p_expression) const;

	static constexpr ResumeAction ACTION_CONTINUE = RESUME_CONTINUE;
	static constexpr ResumeAction ACTION_STEP_OVER = RESUME_STEP_OVER;
	static constexpr ResumeAction ACTION_STEP_IN = RESUME_STEP_IN;
	static constexpr ResumeAction ACTION_STEP_OUT = RESUME_STEP_OUT;
};

} // namespace gdas

#endif // AS_DEBUGGER_H
