#ifndef ANGELSCRIPT_RESOURCE_FORMAT_H
#define ANGELSCRIPT_RESOURCE_FORMAT_H

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>

namespace gdas {

class AngelScriptResourceFormatLoader : public godot::ResourceFormatLoader {
	GDCLASS(AngelScriptResourceFormatLoader, godot::ResourceFormatLoader);

	static godot::Ref<AngelScriptResourceFormatLoader> instance;

protected:
	static void _bind_methods() {}

public:
	static void register_in_godot();
	static void unregister_in_godot();

	godot::PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const godot::StringName &p_type) const override;
	godot::String _get_resource_type(const godot::String &p_path) const override;
	godot::Variant _load(const godot::String &p_path, const godot::String &p_original_path,
			bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

class AngelScriptResourceFormatSaver : public godot::ResourceFormatSaver {
	GDCLASS(AngelScriptResourceFormatSaver, godot::ResourceFormatSaver);

	static godot::Ref<AngelScriptResourceFormatSaver> instance;

protected:
	static void _bind_methods() {}

public:
	static void register_in_godot();
	static void unregister_in_godot();

	godot::Error _save(const godot::Ref<godot::Resource> &p_resource, const godot::String &p_path,
			uint32_t p_flags) override;
	bool _recognize(const godot::Ref<godot::Resource> &p_resource) const override;
	godot::PackedStringArray _get_recognized_extensions(const godot::Ref<godot::Resource> &p_resource) const override;
};

} // namespace gdas

#endif // ANGELSCRIPT_RESOURCE_FORMAT_H
