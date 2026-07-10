#include "angelscript_resource_format.h"

#include "angelscript_language.h"
#include "angelscript_script.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

using namespace godot;

namespace gdas {

Ref<AngelScriptResourceFormatLoader> AngelScriptResourceFormatLoader::instance;
Ref<AngelScriptResourceFormatSaver> AngelScriptResourceFormatSaver::instance;

void AngelScriptResourceFormatLoader::register_in_godot() {
	instance.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(instance);
}

void AngelScriptResourceFormatLoader::unregister_in_godot() {
	if (instance.is_valid()) {
		ResourceLoader::get_singleton()->remove_resource_format_loader(instance);
		instance.unref();
	}
}

PackedStringArray AngelScriptResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray out;
	out.push_back("as");
	return out;
}

bool AngelScriptResourceFormatLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("Script") || p_type == StringName("AngelScriptScript");
}

String AngelScriptResourceFormatLoader::_get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "as" ? "AngelScriptScript" : "";
}

Variant AngelScriptResourceFormatLoader::_load(const String &p_path, const String &p_original_path,
		bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<AngelScriptScript> script;
	script.instantiate();
	String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return Variant();
	}
	script->set_path(p_original_path.is_empty() ? p_path : p_original_path);
	script->set_source_code(source);
	script->reload(false);
	return script;
}

void AngelScriptResourceFormatSaver::register_in_godot() {
	instance.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(instance);
}

void AngelScriptResourceFormatSaver::unregister_in_godot() {
	if (instance.is_valid()) {
		ResourceSaver::get_singleton()->remove_resource_format_saver(instance);
		instance.unref();
	}
}

Error AngelScriptResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path,
		uint32_t p_flags) {
	Ref<AngelScriptScript> script = p_resource;
	if (script.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}
	file->store_string(script->get_source_code());
	if (file->get_error() != OK && file->get_error() != ERR_FILE_EOF) {
		return ERR_CANT_CREATE;
	}
	return OK;
}

bool AngelScriptResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<AngelScriptScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray AngelScriptResourceFormatSaver::_get_recognized_extensions(
		const Ref<Resource> &p_resource) const {
	PackedStringArray out;
	if (_recognize(p_resource)) {
		out.push_back("as");
	}
	return out;
}

} // namespace gdas
