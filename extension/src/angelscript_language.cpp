#include "angelscript_language.h"

#include "angelscript_instance.h"
#include "binding/as_environment.h"
#include "binding/native_classes.h"
#include "binding/script_bases.h"
#include "binding/variant_bridge.h"
#include "debugger/as_debugger.h"
#include "lsp/tooling_server.h"

#include <scriptbuilder/scriptbuilder.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

AngelScriptLanguage *AngelScriptLanguage::singleton = nullptr;

AngelScriptLanguage::AngelScriptLanguage() {
}

AngelScriptLanguage::~AngelScriptLanguage() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

AngelScriptLanguage *AngelScriptLanguage::get_or_create_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(AngelScriptLanguage);
		Engine::get_singleton()->register_script_language(singleton);
	}
	return singleton;
}

void AngelScriptLanguage::free_singleton() {
	if (singleton != nullptr) {
		Engine::get_singleton()->unregister_script_language(singleton);
		memdelete(singleton);
		singleton = nullptr;
	}
}

void AngelScriptLanguage::ensure_initialized() {
	if (initialized) {
		return;
	}
	initialized = true;
	AsEnvironment::create_singleton();
	set_instance_lookup(&AngelScriptInstance::lookup_script_object);
	build_bases_module(AsEnvironment::get_singleton()->get_engine());
}

// ---------------------------------------------------------------------------
// Script registry + shared module rebuild
// ---------------------------------------------------------------------------

void AngelScriptLanguage::register_script(const String &p_path, AngelScriptScript *p_script) {
	if (p_path.is_empty()) {
		return;
	}
	scripts.insert(p_path, p_script);
	module_dirty = true;
}

void AngelScriptLanguage::unregister_script(const String &p_path, AngelScriptScript *p_script) {
	AngelScriptScript **found = scripts.getptr(p_path);
	if (found != nullptr && *found == p_script) {
		scripts.erase(p_path);
		module_dirty = true;
	}
}

// Maps an AngelScript type id to a PropertyInfo-style dictionary.
static Dictionary propinfo_from_typeid(asIScriptEngine *p_engine, int p_type_id, const String &p_name) {
	Dictionary info;
	info["name"] = p_name;
	info["hint"] = 0;
	info["hint_string"] = String();
	info["usage"] = 6; // STORAGE | EDITOR
	info["class_name"] = StringName();

	Variant::Type vtype = bridge_get_variant_type(p_type_id & ~asTYPEID_OBJHANDLE);
	if (vtype != Variant::VARIANT_MAX) {
		info["type"] = int(vtype);
		return info;
	}
	if (!(p_type_id & asTYPEID_MASK_OBJECT)) {
		switch (p_type_id) {
			case asTYPEID_BOOL:
				info["type"] = int(Variant::BOOL);
				break;
			case asTYPEID_FLOAT:
			case asTYPEID_DOUBLE:
				info["type"] = int(Variant::FLOAT);
				break;
			case asTYPEID_VOID:
				info["type"] = int(Variant::NIL);
				break;
			default:
				info["type"] = int(Variant::INT);
				break;
		}
		return info;
	}
	// Variant registered type maps to NIL_IS_VARIANT.
	if (p_type_id == bridge_get_as_type_id(Variant::VARIANT_MAX)) {
		info["type"] = int(Variant::NIL);
		info["usage"] = 6 | PROPERTY_USAGE_NIL_IS_VARIANT;
		return info;
	}
	asITypeInfo *type = p_engine->GetTypeInfoById(p_type_id);
	if (type != nullptr) {
		NativeClassInfo *native = static_cast<NativeClassInfo *>(type->GetUserData(NATIVE_CLASS_USERDATA));
		if (native != nullptr) {
			info["type"] = int(Variant::OBJECT);
			info["class_name"] = native->godot_class;
			return info;
		}
		if (type->GetFlags() & asOBJ_SCRIPT_OBJECT) {
			info["type"] = int(Variant::OBJECT);
			// Best effort: the script base name matches the Godot class.
			info["class_name"] = StringName(type->GetName());
			return info;
		}
	}
	info["type"] = int(Variant::NIL);
	return info;
}

static Dictionary methodinfo_from_function(asIScriptEngine *p_engine, asIScriptFunction *p_func) {
	Dictionary info;
	info["name"] = String::utf8(p_func->GetName());
	info["flags"] = int(METHOD_FLAG_NORMAL);
	Array args;
	for (asUINT i = 0; i < p_func->GetParamCount(); i++) {
		int type_id = 0;
		const char *name = nullptr;
		p_func->GetParam(i, &type_id, nullptr, &name);
		args.push_back(propinfo_from_typeid(p_engine, type_id, name != nullptr ? String::utf8(name) : vformat("arg%d", i)));
	}
	info["args"] = args;
	info["default_args"] = Array();
	info["return"] = propinfo_from_typeid(p_engine, p_func->GetReturnTypeId(), "");
	info["id"] = 0;
	return info;
}

// Walks the class hierarchy to the deepest generated script base and returns
// its name (== the Godot native class), or empty when the type is not a
// Godot-bound class at all.
static StringName find_native_base(asITypeInfo *p_type) {
	asITypeInfo *type = p_type;
	while (type != nullptr) {
		asIScriptModule *module = type->GetModule();
		if (module != nullptr && String::utf8(module->GetName()) == "GodotBases") {
			String name = type->GetName();
			if (name == "GodotObject") {
				return StringName("Object");
			}
			return StringName(name);
		}
		type = type->GetBaseType();
	}
	return StringName();
}

static bool metadata_has(const std::vector<std::string> &p_meta, const char *p_key, String *r_full = nullptr) {
	for (const std::string &entry : p_meta) {
		String meta = String::utf8(entry.c_str()).strip_edges();
		if (meta == p_key || meta.begins_with(String(p_key) + "(") || meta.begins_with(String(p_key) + " ")) {
			if (r_full != nullptr) {
				*r_full = meta;
			}
			return true;
		}
	}
	return false;
}

void AngelScriptLanguage::ensure_module_built() {
	ensure_initialized();
	if (!module_dirty) {
		return;
	}
	module_dirty = false;

	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptEngine *engine = env->get_engine();

	CScriptBuilder builder;
	builder.StartNewModule(engine, AsEnvironment::MAIN_MODULE);
	String header = get_user_module_header();
	{
		CharString utf8 = header.utf8();
		builder.AddSectionFromMemory("<godot-api>", utf8.get_data(), utf8.length());
	}
	for (const KeyValue<String, AngelScriptScript *> &kv : scripts) {
		CharString utf8 = kv.value->get_source().utf8();
		builder.AddSectionFromMemory(kv.key.utf8().get_data(), utf8.get_data(), utf8.length());
	}

	int build_result = builder.BuildModule();
	asIScriptModule *module = engine->GetModule(AsEnvironment::MAIN_MODULE, asGM_ONLY_IF_EXISTS);

	if (build_result < 0 || module == nullptr) {
		for (KeyValue<String, AngelScriptScript *> &kv : scripts) {
			ScriptClassInfo info;
			info.valid = false;
			kv.value->set_placeholder_fallback(true);
			kv.value->set_info(info);
		}
		return;
	}

	// Collect the main class of each script file.
	HashMap<String, asITypeInfo *> path_to_type;
	for (asUINT i = 0; i < module->GetObjectTypeCount(); i++) {
		asITypeInfo *type = module->GetObjectTypeByIndex(i);
		if (find_native_base(type) == StringName()) {
			continue;
		}
		// Where was this class declared?
		const char *section = nullptr;
		asIScriptFunction *marker = nullptr;
		if (type->GetFactoryCount() > 0) {
			marker = type->GetFactoryByIndex(0);
		} else if (type->GetMethodCount() > 0) {
			marker = type->GetMethodByIndex(0);
		}
		if (marker == nullptr) {
			continue;
		}
		marker->GetDeclaredAt(&section, nullptr, nullptr);
		if (section == nullptr) {
			continue;
		}
		String path = String::utf8(section);
		if (!path_to_type.has(path)) {
			path_to_type.insert(path, type);
		}
	}

	for (KeyValue<String, AngelScriptScript *> &kv : scripts) {
		AngelScriptScript *script = kv.value;
		ScriptClassInfo info;

		asITypeInfo **type_ptr = path_to_type.getptr(kv.key);
		if (type_ptr == nullptr) {
			info.valid = false;
			script->set_placeholder_fallback(true);
			script->set_info(info);
			continue;
		}
		asITypeInfo *type = *type_ptr;
		int type_id = type->GetTypeId();

		info.valid = true;
		info.type = type;
		info.class_name = StringName(type->GetName());
		info.native_base = find_native_base(type);
		info.tool = metadata_has(builder.GetMetadataForType(type_id), "tool");

		// User-declared methods and signals: anything declared in a script
		// file (this one or an inherited user class), never generated code.
		for (asUINT m = 0; m < type->GetMethodCount(); m++) {
			// getVirtual=false: virtual method stubs carry no declaration info.
			// CScriptBuilder, however, keys metadata by the *stub*.
			asIScriptFunction *func = type->GetMethodByIndex(m, false);
			asIScriptFunction *stub = type->GetMethodByIndex(m, true);
			const char *section = nullptr;
			int row = 0;
			func->GetDeclaredAt(&section, &row, nullptr);
			if (section == nullptr) {
				continue;
			}
			String section_str = String::utf8(section);
			if (section_str == "GodotBases.gen.as" || section_str == "<godot-api>") {
				continue;
			}
			StringName method_name(func->GetName());
			Dictionary method_info = methodinfo_from_function(engine, func);
			bool is_signal = metadata_has(builder.GetMetadataForTypeMethod(type_id, stub), "signal") ||
					metadata_has(builder.GetMetadataForTypeMethod(type_id, func), "signal");
			if (is_signal) {
				info.signals.insert(method_name, method_info);
			} else {
				info.methods.insert(method_name, method_info);
				if (section_str == kv.key) {
					info.member_lines.insert(method_name, row);
				}
			}
		}

		// Properties: everything except internal fields; exports from metadata.
		for (asUINT p = 0; p < type->GetPropertyCount(); p++) {
			const char *prop_name = nullptr;
			int prop_type_id = 0;
			type->GetProperty(p, &prop_name, &prop_type_id);
			String name = String::utf8(prop_name);
			if (name.begins_with("__")) {
				continue;
			}
			info.property_indices.insert(StringName(name), int(p));

			String export_meta;
			if (metadata_has(builder.GetMetadataForTypeProperty(type_id, int(p)), "export", &export_meta)) {
				Dictionary prop_info = propinfo_from_typeid(engine, prop_type_id, name);
				prop_info["usage"] = int(prop_info["usage"]) | PROPERTY_USAGE_SCRIPT_VARIABLE;
				info.exported_properties.push_back(prop_info);
			}
		}

		// Default values from a throwaway instance (native creation is
		// suppressed so constructors have no side effects).
		if (type->GetFactoryCount() > 0 && !info.exported_properties.is_empty()) {
			set_pending_owner(nullptr);
			set_suppress_native_instantiate(true);
			asIScriptObject *temp = static_cast<asIScriptObject *>(engine->CreateScriptObject(type));
			set_suppress_native_instantiate(false);
			if (temp != nullptr) {
				for (const Dictionary &prop : info.exported_properties) {
					StringName pname = prop.get("name", "");
					const int *idx = info.property_indices.getptr(pname);
					if (idx != nullptr) {
						int prop_type_id = temp->GetPropertyTypeId(asUINT(*idx));
						info.property_defaults.insert(pname,
								as_to_variant(engine, temp->GetAddressOfProperty(asUINT(*idx)), prop_type_id));
					}
				}
				temp->Release();
			}
		}

		script->set_placeholder_fallback(false);
		script->set_info(info);
	}

	UtilityFunctions::print_verbose(vformat("AngelScript: rebuilt %s (%d scripts).",
			String(AsEnvironment::MAIN_MODULE), int64_t(scripts.size())));

	// Push refreshed script classes to connected tooling clients.
	ToolingServer *server = ToolingServer::get_singleton();
	if (server != nullptr) {
		for (const Dictionary &msg : get_script_class_messages()) {
			server->publish_script_class(msg);
		}
	}
}

LocalVector<Dictionary> AngelScriptLanguage::get_script_class_messages() const {
	LocalVector<Dictionary> out;
	for (const KeyValue<String, AngelScriptScript *> &kv : scripts) {
		const ScriptClassInfo &info = kv.value->get_info();
		if (!info.valid) {
			continue;
		}
		Dictionary msg;
		msg["type"] = "type_db_class";
		msg["name"] = String(info.class_name);
		msg["base"] = String(info.native_base);
		msg["native"] = false;
		msg["path"] = kv.key;
		msg["line"] = 1;

		Array methods;
		for (const KeyValue<StringName, Dictionary> &method_kv : info.methods) {
			const Dictionary &m = method_kv.value;
			Dictionary method;
			method["name"] = m.get("name", "");
			Dictionary ret = m.get("return", Dictionary());
			method["return"] = int(ret.get("type", 0)) == 0 ? String("void") : as_type_from_property(ret).trim_prefix("godot::");
			method["static"] = false;
			method["vararg"] = false;
			Array args;
			Array in_args = m.get("args", Array());
			for (int a = 0; a < in_args.size(); a++) {
				Dictionary in_arg = in_args[a];
				Dictionary arg;
				arg["name"] = in_arg.get("name", "");
				arg["type"] = as_type_from_property(in_arg).trim_prefix("godot::");
				arg["default"] = Variant();
				args.push_back(arg);
			}
			method["args"] = args;
			methods.push_back(method);
		}
		msg["methods"] = methods;

		Array properties;
		for (const KeyValue<StringName, int> &prop_kv : info.property_indices) {
			Dictionary prop;
			prop["name"] = String(prop_kv.key);
			prop["type"] = "Variant";
			properties.push_back(prop);
		}
		msg["properties"] = properties;

		Array signals;
		for (const KeyValue<StringName, Dictionary> &sig_kv : info.signals) {
			Dictionary sig;
			sig["name"] = String(sig_kv.key);
			sig["args"] = Array();
			signals.push_back(sig);
		}
		msg["signals"] = signals;
		msg["constants"] = Array();
		msg["enums"] = Array();
		out.push_back(msg);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Identity / editor basics
// ---------------------------------------------------------------------------

String AngelScriptLanguage::_get_name() const {
	return "AngelScript";
}

void AngelScriptLanguage::_init() {
	ensure_initialized();
}

String AngelScriptLanguage::_get_type() const {
	return "AngelScriptScript";
}

String AngelScriptLanguage::_get_extension() const {
	return "as";
}

void AngelScriptLanguage::_finish() {
}

PackedStringArray AngelScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray out;
	out.push_back("as");
	return out;
}

PackedStringArray AngelScriptLanguage::_get_reserved_words() const {
	static const char *words[] = {
		"and", "abstract", "auto", "bool", "break", "case", "cast", "catch", "class",
		"const", "continue", "default", "do", "double", "else", "enum", "explicit",
		"external", "false", "final", "float", "for", "foreach", "from", "funcdef",
		"function", "get", "if", "import", "in", "inout", "int", "interface", "int8",
		"int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or",
		"out", "override", "private", "property", "protected", "return", "set",
		"shared", "super", "switch", "this", "true", "try", "typedef", "uint",
		"uint8", "uint16", "uint32", "uint64", "void", "while", "xor", nullptr
	};
	PackedStringArray out;
	for (int i = 0; words[i] != nullptr; i++) {
		out.push_back(words[i]);
	}
	return out;
}

bool AngelScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const char *keywords[] = {
		"break", "case", "continue", "default", "do", "else", "for", "foreach",
		"if", "return", "switch", "try", "catch", "while", nullptr
	};
	for (int i = 0; keywords[i] != nullptr; i++) {
		if (p_keyword == keywords[i]) {
			return true;
		}
	}
	return false;
}

PackedStringArray AngelScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray out;
	out.push_back("//");
	out.push_back("/* */");
	return out;
}

PackedStringArray AngelScriptLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray out;
	out.push_back("///");
	out.push_back("/** */");
	return out;
}

PackedStringArray AngelScriptLanguage::_get_string_delimiters() const {
	PackedStringArray out;
	out.push_back("\" \"");
	out.push_back("' '");
	out.push_back("\"\"\" \"\"\"");
	return out;
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

static const char *DEFAULT_TEMPLATE =
		"class _CLASS_ : _BASE_ {\n"
		"_TS_// Called when the node enters the scene tree for the first time.\n"
		"_TS_void _ready() {\n"
		"_TS_}\n"
		"\n"
		"_TS_// Called every frame. 'delta' is the elapsed time since the previous frame.\n"
		"_TS_void _process(double delta) {\n"
		"_TS_}\n"
		"}\n";

static const char *EMPTY_TEMPLATE =
		"class _CLASS_ : _BASE_ {\n"
		"}\n";

Ref<Script> AngelScriptLanguage::_make_template(const String &p_template, const String &p_class_name,
		const String &p_base_class_name) const {
	String source = p_template;
	if (source.is_empty()) {
		source = DEFAULT_TEMPLATE;
	}
	String class_name = p_class_name.to_pascal_case();
	source = source.replace("_CLASS_", class_name)
					 .replace("_BASE_", p_base_class_name)
					 .replace("_TS_", "\t");
	Ref<AngelScriptScript> script;
	script.instantiate();
	script->set_source_code(source);
	return script;
}

TypedArray<Dictionary> AngelScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	TypedArray<Dictionary> out;
	Dictionary default_template;
	default_template["inherit"] = String(p_object);
	default_template["name"] = "Node";
	default_template["description"] = "Base template with _ready and _process callbacks";
	default_template["content"] = String(DEFAULT_TEMPLATE);
	default_template["id"] = 0;
	default_template["origin"] = 0;
	out.push_back(default_template);

	Dictionary empty_template;
	empty_template["inherit"] = String(p_object);
	empty_template["name"] = "Empty";
	empty_template["description"] = "Empty class";
	empty_template["content"] = String(EMPTY_TEMPLATE);
	empty_template["id"] = 1;
	empty_template["origin"] = 0;
	out.push_back(empty_template);
	return out;
}

// ---------------------------------------------------------------------------
// Validation / editing
// ---------------------------------------------------------------------------

Dictionary AngelScriptLanguage::_validate(const String &p_script, const String &p_path,
		bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings,
		bool p_validate_safe_lines) const {
	AngelScriptLanguage *self = const_cast<AngelScriptLanguage *>(this);
	self->ensure_initialized();

	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptEngine *engine = env->get_engine();

	String validate_path = p_path.is_empty() ? String("<validate>") : p_path;

	CScriptBuilder builder;
	builder.StartNewModule(engine, "__validate__");
	{
		String header = get_user_module_header();
		CharString utf8 = header.utf8();
		builder.AddSectionFromMemory("<godot-api>", utf8.get_data(), utf8.length());
	}
	// Other project scripts provide cross-file classes.
	for (const KeyValue<String, AngelScriptScript *> &kv : scripts) {
		if (kv.key == validate_path) {
			continue;
		}
		CharString utf8 = kv.value->get_source().utf8();
		builder.AddSectionFromMemory(kv.key.utf8().get_data(), utf8.get_data(), utf8.length());
	}
	{
		CharString utf8 = p_script.utf8();
		builder.AddSectionFromMemory(validate_path.utf8().get_data(), utf8.get_data(), utf8.length());
	}

	env->begin_message_capture();
	int build_result = builder.BuildModule();
	LocalVector<AsMessage> messages = env->end_message_capture();
	engine->DiscardModule("__validate__");

	Dictionary result;
	result["valid"] = build_result >= 0;

	if (p_validate_errors) {
		Array errors;
		for (const AsMessage &msg : messages) {
			if (msg.type != asMSGTYPE_ERROR) {
				continue;
			}
			// Only report rows from the file being validated; errors in other
			// files get attached to line 1 with a path prefix.
			Dictionary error;
			if (msg.section == validate_path) {
				error["line"] = msg.row;
				error["column"] = MAX(1, msg.col);
				error["message"] = msg.message;
			} else {
				error["line"] = 1;
				error["column"] = 1;
				error["message"] = vformat("(%s:%d) %s", msg.section, msg.row, msg.message);
			}
			errors.push_back(error);
		}
		result["errors"] = errors;
	}
	if (p_validate_warnings) {
		Array warnings;
		for (const AsMessage &msg : messages) {
			if (msg.type != asMSGTYPE_WARNING || msg.section != validate_path) {
				continue;
			}
			Dictionary warning;
			warning["start_line"] = msg.row;
			warning["end_line"] = msg.row;
			warning["code"] = 0;
			warning["string_code"] = "WARNING";
			warning["message"] = msg.message;
			warnings.push_back(warning);
		}
		result["warnings"] = warnings;
	}
	if (p_validate_functions) {
		result["functions"] = PackedStringArray();
	}
	return result;
}

Object *AngelScriptLanguage::_create_script() const {
	return memnew(AngelScriptScript);
}

int32_t AngelScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	PackedStringArray lines = p_code.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String &line = lines[i];
		int idx = line.find(p_function);
		if (idx >= 0 && line.find("(", idx) > idx && !line.strip_edges().begins_with("//")) {
			return i + 1;
		}
	}
	return -1;
}

String AngelScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name,
		const PackedStringArray &p_function_args) const {
	String args;
	for (int i = 0; i < p_function_args.size(); i++) {
		if (i > 0) {
			args += ", ";
		}
		// Godot passes "name:Type" pairs.
		String arg = p_function_args[i];
		String name = arg.get_slice(":", 0);
		String type = arg.contains(":") ? arg.get_slice(":", 1) : String("Variant");
		String as_type = as_type_from_godot_name(type);
		if (as_type.begins_with("godot::")) {
			as_type = as_type.trim_prefix("godot::").trim_suffix("@") + "@";
		}
		args += as_type + " " + name;
	}
	return vformat("\tvoid %s(%s) {\n\t}\n", p_function_name, args);
}

Dictionary AngelScriptLanguage::_complete_code(const String &p_code, const String &p_path,
		Object *p_owner) const {
	// v1: keyword + type completion; member-aware completion is provided by
	// the VS Code language server. See docs/ROADMAP.md.
	Dictionary result;
	result["result"] = int(OK);
	result["force"] = false;
	result["call_hint"] = String();
	Array options;

	for (const String &word : _get_reserved_words()) {
		Dictionary option;
		option["kind"] = 10; // KEYWORD
		option["display"] = word;
		option["insert_text"] = word;
		option["font_color"] = Color();
		option["icon"] = Ref<Resource>();
		option["default_value"] = Variant();
		option["location"] = 1 << 10; // OTHER
		options.push_back(option);
	}
	for (const KeyValue<StringName, ClassSpec> &kv : get_class_specs()) {
		Dictionary option;
		option["kind"] = 0; // CLASS
		option["display"] = String(kv.key);
		option["insert_text"] = String(kv.key);
		option["font_color"] = Color();
		option["icon"] = Ref<Resource>();
		option["default_value"] = Variant();
		option["location"] = 1 << 10;
		options.push_back(option);
	}
	result["options"] = options;
	return result;
}

Dictionary AngelScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol,
		const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = int(ERR_CANT_RESOLVE);
	result["type"] = 0;
	AngelScriptLanguage *self = const_cast<AngelScriptLanguage *>(this);
	self->ensure_module_built();

	// Script classes: jump to their file.
	for (const KeyValue<String, AngelScriptScript *> &kv : scripts) {
		const ScriptClassInfo &info = kv.value->get_info();
		if (String(info.class_name) == p_symbol) {
			result["result"] = int(OK);
			result["type"] = 0; // SCRIPT_LOCATION
			result["script_path"] = kv.key;
			result["location"] = 1;
			return result;
		}
		const int *line = info.member_lines.getptr(StringName(p_symbol));
		if (line != nullptr && kv.key == p_path) {
			result["result"] = int(OK);
			result["type"] = 0;
			result["script_path"] = kv.key;
			result["location"] = *line;
			return result;
		}
	}
	// Native classes: point at the class documentation.
	if (is_native_class(StringName(p_symbol))) {
		result["result"] = int(OK);
		result["type"] = 1; // CLASS
		result["class_name"] = p_symbol;
		return result;
	}
	return result;
}

String AngelScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line,
		int32_t p_to_line) const {
	return p_code;
}

// ---------------------------------------------------------------------------
// Debugging
// ---------------------------------------------------------------------------

String AngelScriptLanguage::_debug_get_error() const {
	AsEnvironment *env = AsEnvironment::get_singleton();
	if (env == nullptr) {
		return String();
	}
	asIScriptContext *ctx = env->get_active_context();
	if (ctx != nullptr && ctx->GetState() == asEXECUTION_EXCEPTION) {
		return String::utf8(ctx->GetExceptionString());
	}
	return String();
}

int32_t AngelScriptLanguage::_debug_get_stack_level_count() const {
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	return ctx != nullptr ? int32_t(ctx->GetCallstackSize()) : 0;
}

int32_t AngelScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	return ctx != nullptr ? ctx->GetLineNumber(asUINT(p_level)) : 0;
}

String AngelScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	if (ctx == nullptr) {
		return String();
	}
	asIScriptFunction *func = ctx->GetFunction(asUINT(p_level));
	return func != nullptr ? String::utf8(func->GetName()) : String();
}

String AngelScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	if (ctx == nullptr) {
		return String();
	}
	const char *section = nullptr;
	ctx->GetLineNumber(asUINT(p_level), nullptr, &section);
	return section != nullptr ? String::utf8(section) : String();
}

Dictionary AngelScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems,
		int32_t p_max_depth) {
	Dictionary out;
	PackedStringArray names;
	Array values;
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	if (ctx != nullptr) {
		asUINT level = asUINT(p_level);
		for (int i = 0; i < ctx->GetVarCount(level); i++) {
			const char *name = nullptr;
			int type_id = 0;
			if (ctx->GetVar(asUINT(i), level, &name, &type_id) < 0) {
				continue;
			}
			if (name == nullptr || name[0] == '\0' || !ctx->IsVarInScope(asUINT(i), level)) {
				continue;
			}
			names.push_back(String::utf8(name));
			values.push_back(as_to_variant(ctx->GetEngine(), ctx->GetAddressOfVar(asUINT(i), level), type_id));
		}
	}
	out["locals"] = names;
	out["values"] = values;
	return out;
}

Dictionary AngelScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems,
		int32_t p_max_depth) {
	Dictionary out;
	PackedStringArray names;
	Array values;
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	if (ctx != nullptr) {
		asIScriptObject *object = static_cast<asIScriptObject *>(ctx->GetThisPointer(asUINT(p_level)));
		if (object != nullptr) {
			for (asUINT i = 0; i < object->GetPropertyCount(); i++) {
				String name = String::utf8(object->GetPropertyName(i));
				if (name.begins_with("__")) {
					continue;
				}
				names.push_back(name);
				values.push_back(as_to_variant(ctx->GetEngine(), object->GetAddressOfProperty(i),
						object->GetPropertyTypeId(i)));
			}
		}
	}
	out["members"] = names;
	out["values"] = values;
	return out;
}

void *AngelScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

Dictionary AngelScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary out;
	out["globals"] = PackedStringArray();
	out["values"] = Array();
	return out;
}

String AngelScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level,
		const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}

TypedArray<Dictionary> AngelScriptLanguage::_debug_get_current_stack_info() {
	TypedArray<Dictionary> out;
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env != nullptr ? env->get_active_context() : nullptr;
	if (ctx == nullptr) {
		return out;
	}
	for (asUINT level = 0; level < ctx->GetCallstackSize(); level++) {
		const char *section = nullptr;
		int line = ctx->GetLineNumber(level, nullptr, &section);
		asIScriptFunction *func = ctx->GetFunction(level);
		Dictionary frame;
		frame["file"] = section != nullptr ? String::utf8(section) : String();
		frame["func"] = func != nullptr ? String::utf8(func->GetName()) : String();
		frame["line"] = line;
		out.push_back(frame);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Reload
// ---------------------------------------------------------------------------

void AngelScriptLanguage::_reload_all_scripts() {
	module_dirty = true;
	ensure_module_built();
}

void AngelScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	module_dirty = true;
	ensure_module_built();
}

void AngelScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	module_dirty = true;
	ensure_module_built();
}

TypedArray<Dictionary> AngelScriptLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}

Dictionary AngelScriptLanguage::_get_public_constants() const {
	return Dictionary();
}

TypedArray<Dictionary> AngelScriptLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}

void AngelScriptLanguage::_frame() {
	if (module_dirty) {
		ensure_module_built();
	}
	ToolingServer *server = ToolingServer::get_singleton();
	if (server != nullptr) {
		server->poll();
	}
}

bool AngelScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == _get_type();
}

Dictionary AngelScriptLanguage::_get_global_class_name(const String &p_path) const {
	// Global class registration (class_name equivalent) is on the roadmap.
	return Dictionary();
}

} // namespace gdas
