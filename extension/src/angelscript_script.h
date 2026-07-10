#ifndef ANGELSCRIPT_SCRIPT_H
#define ANGELSCRIPT_SCRIPT_H

#include <angelscript.h>

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>

namespace gdas {

// Metadata for the main class of one .as file, refreshed on module rebuild.
struct ScriptClassInfo {
	bool valid = false;
	bool tool = false;
	godot::StringName class_name;
	godot::StringName native_base; // e.g. CharacterBody2D
	asITypeInfo *type = nullptr; // borrowed from the main module

	godot::HashMap<godot::StringName, godot::Dictionary> methods; // user-declared
	godot::HashMap<godot::StringName, godot::Dictionary> signals;
	godot::LocalVector<godot::Dictionary> exported_properties;
	godot::HashMap<godot::StringName, godot::Variant> property_defaults;
	godot::HashMap<godot::StringName, int> property_indices; // name -> asIScriptObject prop index
	godot::HashMap<godot::StringName, int> member_lines;
};

class AngelScriptScript : public godot::ScriptExtension {
	GDCLASS(AngelScriptScript, godot::ScriptExtension);

	friend class AngelScriptLanguage;

	godot::String source_code;
	ScriptClassInfo info;
	bool placeholder_fallback = false;

	godot::HashSet<void *> placeholders;

	void update_placeholders();

protected:
	static void _bind_methods() {}

public:
	// ScriptExtension
	bool _editor_can_reload_from_file() override { return true; }
	void _placeholder_erased(void *p_placeholder) override;
	bool _can_instantiate() const override;
	godot::Ref<godot::Script> _get_base_script() const override;
	godot::StringName _get_global_name() const override;
	bool _inherits_script(const godot::Ref<godot::Script> &p_script) const override;
	godot::StringName _get_instance_base_type() const override;
	void *_instance_create(godot::Object *p_for_object) const override;
	void *_placeholder_instance_create(godot::Object *p_for_object) const override;
	bool _instance_has(godot::Object *p_object) const override;
	bool _has_source_code() const override { return !source_code.is_empty(); }
	godot::String _get_source_code() const override { return source_code; }
	void _set_source_code(const godot::String &p_code) override;
	godot::Error _reload(bool p_keep_state) override;
	godot::StringName _get_doc_class_name() const override { return info.class_name; }
	godot::TypedArray<godot::Dictionary> _get_documentation() const override;
	godot::String _get_class_icon_path() const override { return godot::String(); }
	bool _has_method(const godot::StringName &p_method) const override;
	bool _has_static_method(const godot::StringName &p_method) const override { return false; }
	godot::Variant _get_script_method_argument_count(const godot::StringName &p_method) const override;
	godot::Dictionary _get_method_info(const godot::StringName &p_method) const override;
	bool _is_tool() const override { return info.tool; }
	bool _is_valid() const override { return info.valid; }
	bool _is_abstract() const override { return false; }
	godot::ScriptLanguage *_get_language() const override;
	bool _has_script_signal(const godot::StringName &p_signal) const override;
	godot::TypedArray<godot::Dictionary> _get_script_signal_list() const override;
	bool _has_property_default_value(const godot::StringName &p_property) const override;
	godot::Variant _get_property_default_value(const godot::StringName &p_property) const override;
	void _update_exports() override;
	godot::TypedArray<godot::Dictionary> _get_script_method_list() const override;
	godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
	int32_t _get_member_line(const godot::StringName &p_member) const override;
	godot::TypedArray<godot::StringName> _get_members() const override;
	bool _is_placeholder_fallback_enabled() const override { return placeholder_fallback; }
	godot::Variant _get_rpc_config() const override { return godot::Dictionary(); }
	godot::Dictionary _get_constants() const override { return godot::Dictionary(); }

	// Internal
	const ScriptClassInfo &get_info() const { return info; }
	void set_info(const ScriptClassInfo &p_info);
	void set_placeholder_fallback(bool p_enabled) { placeholder_fallback = p_enabled; }
	godot::String get_source() const { return source_code; }

	AngelScriptScript();
	~AngelScriptScript() override;
};

} // namespace gdas

#endif // ANGELSCRIPT_SCRIPT_H
