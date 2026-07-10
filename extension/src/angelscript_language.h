#ifndef ANGELSCRIPT_LANGUAGE_H
#define ANGELSCRIPT_LANGUAGE_H

#include "angelscript_script.h"

#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/templates/hash_map.hpp>

namespace gdas {

class AngelScriptLanguage : public godot::ScriptLanguageExtension {
	GDCLASS(AngelScriptLanguage, godot::ScriptLanguageExtension);

	static AngelScriptLanguage *singleton;

	// path -> script; scripts register on load so the shared module can be
	// rebuilt from every source file at once.
	godot::HashMap<godot::String, AngelScriptScript *> scripts;
	bool module_dirty = false;
	bool initialized = false;

protected:
	static void _bind_methods() {}

public:
	static AngelScriptLanguage *get_singleton() { return singleton; }
	static AngelScriptLanguage *get_or_create_singleton();
	static void free_singleton();

	void ensure_initialized();

	void register_script(const godot::String &p_path, AngelScriptScript *p_script);
	void unregister_script(const godot::String &p_path, AngelScriptScript *p_script);
	void mark_module_dirty() { module_dirty = true; }
	// Rebuilds the shared module from all registered scripts if dirty.
	void ensure_module_built();

	// type_db_class messages for every script class (see docs/PROTOCOL.md).
	godot::LocalVector<godot::Dictionary> get_script_class_messages() const;

	// ScriptLanguageExtension
	godot::String _get_name() const override;
	void _init() override;
	godot::String _get_type() const override;
	godot::String _get_extension() const override;
	void _finish() override;

	godot::PackedStringArray _get_reserved_words() const override;
	bool _is_control_flow_keyword(const godot::String &p_keyword) const override;
	godot::PackedStringArray _get_comment_delimiters() const override;
	godot::PackedStringArray _get_doc_comment_delimiters() const override;
	godot::PackedStringArray _get_string_delimiters() const override;

	godot::Ref<godot::Script> _make_template(const godot::String &p_template,
			const godot::String &p_class_name, const godot::String &p_base_class_name) const override;
	godot::TypedArray<godot::Dictionary> _get_built_in_templates(const godot::StringName &p_object) const override;
	bool _is_using_templates() override { return true; }

	godot::Dictionary _validate(const godot::String &p_script, const godot::String &p_path,
			bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings,
			bool p_validate_safe_lines) const override;
	godot::String _validate_path(const godot::String &p_path) const override { return godot::String(); }
	godot::Object *_create_script() const override;

	bool _has_named_classes() const override { return false; }
	bool _supports_builtin_mode() const override { return false; }
	bool _supports_documentation() const override { return false; }
	bool _can_inherit_from_file() const override { return false; }
	int32_t _find_function(const godot::String &p_function, const godot::String &p_code) const override;
	godot::String _make_function(const godot::String &p_class_name, const godot::String &p_function_name,
			const godot::PackedStringArray &p_function_args) const override;
	bool _can_make_function() const override { return true; }
	godot::Error _open_in_external_editor(const godot::Ref<godot::Script> &p_script,
			int32_t p_line, int32_t p_column) override { return godot::ERR_UNAVAILABLE; }
	bool _overrides_external_editor() override { return false; }

	godot::Dictionary _complete_code(const godot::String &p_code, const godot::String &p_path,
			godot::Object *p_owner) const override;
	godot::Dictionary _lookup_code(const godot::String &p_code, const godot::String &p_symbol,
			const godot::String &p_path, godot::Object *p_owner) const override;
	godot::String _auto_indent_code(const godot::String &p_code, int32_t p_from_line,
			int32_t p_to_line) const override;

	void _add_global_constant(const godot::StringName &p_name, const godot::Variant &p_value) override {}
	void _add_named_global_constant(const godot::StringName &p_name, const godot::Variant &p_value) override {}
	void _remove_named_global_constant(const godot::StringName &p_name) override {}

	void _thread_enter() override {}
	void _thread_exit() override {}

	godot::String _debug_get_error() const override;
	int32_t _debug_get_stack_level_count() const override;
	int32_t _debug_get_stack_level_line(int32_t p_level) const override;
	godot::String _debug_get_stack_level_function(int32_t p_level) const override;
	godot::String _debug_get_stack_level_source(int32_t p_level) const override;
	godot::Dictionary _debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems,
			int32_t p_max_depth) override;
	godot::Dictionary _debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems,
			int32_t p_max_depth) override;
	void *_debug_get_stack_level_instance(int32_t p_level) override;
	godot::Dictionary _debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) override;
	godot::String _debug_parse_stack_level_expression(int32_t p_level, const godot::String &p_expression,
			int32_t p_max_subitems, int32_t p_max_depth) override;
	godot::TypedArray<godot::Dictionary> _debug_get_current_stack_info() override;

	void _reload_all_scripts() override;
	void _reload_scripts(const godot::Array &p_scripts, bool p_soft_reload) override;
	void _reload_tool_script(const godot::Ref<godot::Script> &p_script, bool p_soft_reload) override;

	godot::PackedStringArray _get_recognized_extensions() const override;
	godot::TypedArray<godot::Dictionary> _get_public_functions() const override;
	godot::Dictionary _get_public_constants() const override;
	godot::TypedArray<godot::Dictionary> _get_public_annotations() const override;

	void _profiling_start() override {}
	void _profiling_stop() override {}
	void _profiling_set_save_native_calls(bool p_enable) override {}
	int32_t _profiling_get_accumulated_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array,
			int32_t p_info_max) override { return 0; }
	int32_t _profiling_get_frame_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array,
			int32_t p_info_max) override { return 0; }

	void _frame() override;

	bool _handles_global_class_type(const godot::String &p_type) const override;
	godot::Dictionary _get_global_class_name(const godot::String &p_path) const override;

	AngelScriptLanguage();
	~AngelScriptLanguage() override;
};

} // namespace gdas

#endif // ANGELSCRIPT_LANGUAGE_H
