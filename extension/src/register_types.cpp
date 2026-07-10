#include "angelscript_instance.h"
#include "angelscript_language.h"
#include "angelscript_resource_format.h"
#include "angelscript_script.h"
#include "binding/as_environment.h"
#include "binding/script_bases.h"
#include "debugger/as_debugger.h"
#include "lsp/tooling_server.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace gdas {

void native_classes_free_bind_data();

static void initialize_extension(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_ABSTRACT_CLASS(AngelScriptLanguage);
	GDREGISTER_CLASS(AngelScriptScript);
	GDREGISTER_ABSTRACT_CLASS(AngelScriptResourceFormatLoader);
	GDREGISTER_ABSTRACT_CLASS(AngelScriptResourceFormatSaver);

	AngelScriptLanguage::get_or_create_singleton();
	AngelScriptResourceFormatLoader::register_in_godot();
	AngelScriptResourceFormatSaver::register_in_godot();

#ifdef DEBUG_ENABLED
	AsDebugger::create_singleton();
	ToolingServer::create_singleton();
#endif
}

static void deinitialize_extension(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
#ifdef DEBUG_ENABLED
	ToolingServer::free_singleton();
	AsDebugger::free_singleton();
#endif
	AngelScriptResourceFormatSaver::unregister_in_godot();
	AngelScriptResourceFormatLoader::unregister_in_godot();
	AngelScriptLanguage::free_singleton();
	AsEnvironment::free_singleton();
	script_bases_clear_cache();
	native_classes_free_bind_data();
}

} // namespace gdas

extern "C" GDExtensionBool GDE_EXPORT godot_angelscript_entrypoint(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(gdas::initialize_extension);
	init_obj.register_terminator(gdas::deinitialize_extension);
	init_obj.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
