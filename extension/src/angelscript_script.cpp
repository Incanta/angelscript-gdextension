#include "angelscript_script.h"

#include "angelscript_instance.h"
#include "angelscript_language.h"
#include "binding/as_environment.h"
#include "binding/script_bases.h"

#include <godot_cpp/classes/engine.hpp>

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

AngelScriptScript::AngelScriptScript() {
}

AngelScriptScript::~AngelScriptScript() {
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	if (lang != nullptr) {
		lang->unregister_script(get_path(), this);
	}
}

void AngelScriptScript::set_info(const ScriptClassInfo &p_info) {
	info = p_info;
	update_placeholders();
}

void AngelScriptScript::_set_source_code(const String &p_code) {
	source_code = p_code;
}

Error AngelScriptScript::_reload(bool p_keep_state) {
	AngelScriptLanguage *lang = AngelScriptLanguage::get_or_create_singleton();
	lang->register_script(get_path(), this);
	lang->mark_module_dirty();
	lang->ensure_module_built();
	return info.valid ? OK : ERR_PARSE_ERROR;
}

bool AngelScriptScript::_can_instantiate() const {
	if (!info.valid || info.type == nullptr) {
		return false;
	}
	bool is_editor = Engine::get_singleton()->is_editor_hint();
	return !is_editor || info.tool;
}

Ref<Script> AngelScriptScript::_get_base_script() const {
	// Base scripts across files are on the roadmap; the class hierarchy lives
	// inside the shared module.
	return Ref<Script>();
}

StringName AngelScriptScript::_get_global_name() const {
	return StringName();
}

bool AngelScriptScript::_inherits_script(const Ref<Script> &p_script) const {
	const AngelScriptScript *other = Object::cast_to<AngelScriptScript>(p_script.ptr());
	if (other == nullptr || info.type == nullptr || other->info.type == nullptr) {
		return false;
	}
	return info.type == other->info.type || info.type->DerivesFrom(other->info.type);
}

StringName AngelScriptScript::_get_instance_base_type() const {
	return info.native_base;
}

void *AngelScriptScript::_instance_create(Object *p_for_object) const {
	if (!info.valid || info.type == nullptr) {
		return nullptr;
	}
	AsEnvironment *env = AsEnvironment::get_singleton();
	set_pending_owner(p_for_object);
	asIScriptObject *object = static_cast<asIScriptObject *>(env->get_engine()->CreateScriptObject(info.type));
	set_pending_owner(nullptr);
	if (object == nullptr) {
		ERR_PRINT(vformat("AngelScript: failed to instantiate %s.", String(info.class_name)));
		return nullptr;
	}

	AngelScriptInstance *instance = memnew(AngelScriptInstance(p_for_object,
			Ref<AngelScriptScript>(this), object));
	GDExtensionScriptInstancePtr script_instance = internal::gdextension_interface_script_instance_create3(
			AngelScriptInstance::get_instance_info(), instance);
	return script_instance;
}

void *AngelScriptScript::_placeholder_instance_create(Object *p_for_object) const {
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	GDExtensionScriptInstancePtr placeholder = internal::gdextension_interface_placeholder_script_instance_create(
			lang->_owner, const_cast<AngelScriptScript *>(this)->_owner, p_for_object->_owner);
	const_cast<AngelScriptScript *>(this)->placeholders.insert(placeholder);
	const_cast<AngelScriptScript *>(this)->update_placeholders();
	return placeholder;
}

void AngelScriptScript::_placeholder_erased(void *p_placeholder) {
	placeholders.erase(p_placeholder);
}

void AngelScriptScript::update_placeholders() {
	if (placeholders.is_empty()) {
		return;
	}
	Array properties;
	Dictionary values;
	for (const Dictionary &prop : info.exported_properties) {
		properties.push_back(prop);
		StringName name = prop.get("name", "");
		const Variant *def = info.property_defaults.getptr(name);
		values[name] = def != nullptr ? *def : Variant();
	}
	for (void *placeholder : placeholders) {
		internal::gdextension_interface_placeholder_script_instance_update(placeholder,
				const_cast<Array &>(properties)._native_ptr(), const_cast<Dictionary &>(values)._native_ptr());
	}
}

bool AngelScriptScript::_instance_has(Object *p_object) const {
	AngelScriptInstance **found = AngelScriptInstance::owner_map.getptr(p_object);
	return found != nullptr && (*found)->script.ptr() == this;
}

TypedArray<Dictionary> AngelScriptScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}

bool AngelScriptScript::_has_method(const StringName &p_method) const {
	return info.methods.has(p_method);
}

Variant AngelScriptScript::_get_script_method_argument_count(const StringName &p_method) const {
	const Dictionary *method = info.methods.getptr(p_method);
	if (method == nullptr) {
		return Variant();
	}
	return Array(method->get("args", Array())).size();
}

Dictionary AngelScriptScript::_get_method_info(const StringName &p_method) const {
	const Dictionary *method = info.methods.getptr(p_method);
	return method != nullptr ? *method : Dictionary();
}

ScriptLanguage *AngelScriptScript::_get_language() const {
	return AngelScriptLanguage::get_singleton();
}

bool AngelScriptScript::_has_script_signal(const StringName &p_signal) const {
	return info.signals.has(p_signal);
}

TypedArray<Dictionary> AngelScriptScript::_get_script_signal_list() const {
	TypedArray<Dictionary> out;
	for (const KeyValue<StringName, Dictionary> &kv : info.signals) {
		out.push_back(kv.value);
	}
	return out;
}

bool AngelScriptScript::_has_property_default_value(const StringName &p_property) const {
	return info.property_defaults.has(p_property);
}

Variant AngelScriptScript::_get_property_default_value(const StringName &p_property) const {
	const Variant *def = info.property_defaults.getptr(p_property);
	return def != nullptr ? *def : Variant();
}

void AngelScriptScript::_update_exports() {
	update_placeholders();
}

TypedArray<Dictionary> AngelScriptScript::_get_script_method_list() const {
	TypedArray<Dictionary> out;
	for (const KeyValue<StringName, Dictionary> &kv : info.methods) {
		out.push_back(kv.value);
	}
	return out;
}

TypedArray<Dictionary> AngelScriptScript::_get_script_property_list() const {
	TypedArray<Dictionary> out;
	for (const Dictionary &prop : info.exported_properties) {
		out.push_back(prop);
	}
	return out;
}

int32_t AngelScriptScript::_get_member_line(const StringName &p_member) const {
	const int *line = info.member_lines.getptr(p_member);
	return line != nullptr ? *line : -1;
}

TypedArray<StringName> AngelScriptScript::_get_members() const {
	TypedArray<StringName> out;
	for (const KeyValue<StringName, int> &kv : info.property_indices) {
		out.push_back(kv.key);
	}
	return out;
}

} // namespace gdas
