#include "script_bases.h"

#include "as_environment.h"
#include "native_classes.h"
#include "variant_bridge.h"

#include <scriptbuilder/scriptbuilder.h>
#include <scripthandle/scripthandle.h>

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

static InstanceLookupFn instance_lookup = nullptr;
static thread_local Object *pending_owner = nullptr;
static thread_local bool suppress_native_instantiate = false;
static HashMap<StringName, asITypeInfo *> base_type_cache;
// Heap-allocated: godot-cpp types must not be constructed during dlopen
// static initialization (engine bindings are not resolved yet).
static String *cached_user_header = nullptr;
static int cached_user_header_lines = 0;
// Classes excluded from script-space generation because their name is taken
// by a singleton property accessor (Input, Engine, OS, ...).
static HashSet<StringName> singleton_classes;

void set_instance_lookup(InstanceLookupFn p_fn) {
	instance_lookup = p_fn;
}

void set_pending_owner(Object *p_object) {
	pending_owner = p_object;
}

void set_suppress_native_instantiate(bool p_suppress) {
	suppress_native_instantiate = p_suppress;
}

void script_bases_clear_cache() {
	base_type_cache.clear();
	if (cached_user_header != nullptr) {
		memdelete(cached_user_header);
		cached_user_header = nullptr;
	}
	cached_user_header_lines = 0;
	instance_lookup = nullptr;
	pending_owner = nullptr;
}

// ---------------------------------------------------------------------------
// Wrap helpers
// ---------------------------------------------------------------------------

asITypeInfo *find_script_base_type(const StringName &p_class) {
	asITypeInfo **cached = base_type_cache.getptr(p_class);
	if (cached != nullptr) {
		return *cached;
	}
	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptModule *bases = env->get_engine()->GetModule("GodotBases", asGM_ONLY_IF_EXISTS);
	if (bases == nullptr) {
		return nullptr;
	}
	const HashMap<StringName, ClassSpec> &specs = get_class_specs();
	StringName cls = p_class;
	while (cls != StringName()) {
		asITypeInfo *type = bases->GetTypeInfoByDecl(String(cls).utf8().get_data());
		if (type != nullptr) {
			base_type_cache.insert(p_class, type);
			return type;
		}
		const ClassSpec *spec = specs.getptr(cls);
		if (spec == nullptr) {
			break;
		}
		cls = spec->parent;
	}
	base_type_cache.insert(p_class, nullptr);
	return nullptr;
}

asIScriptObject *wrap_object(Object *p_object) {
	if (p_object == nullptr) {
		return nullptr;
	}
	if (instance_lookup != nullptr) {
		asIScriptObject *live = instance_lookup(p_object);
		if (live != nullptr) {
			live->AddRef();
			return live;
		}
	}
	asITypeInfo *type = find_script_base_type(p_object->get_class());
	if (type == nullptr) {
		return nullptr;
	}
	AsEnvironment *env = AsEnvironment::get_singleton();
	pending_owner = p_object;
	asIScriptObject *wrapper = static_cast<asIScriptObject *>(env->get_engine()->CreateScriptObject(type));
	pending_owner = nullptr;
	return wrapper;
}

void *object_to_as_handle(Object *p_object, asITypeInfo *p_target) {
	if (p_object == nullptr || p_target == nullptr) {
		return nullptr;
	}
	if (p_target->GetUserData(NATIVE_CLASS_USERDATA) != nullptr) {
		NativeClassInfo *info = static_cast<NativeClassInfo *>(p_target->GetUserData(NATIVE_CLASS_USERDATA));
		if (!p_object->is_class(info->godot_class)) {
			return nullptr;
		}
		return p_object;
	}
	if (p_target->GetFlags() & asOBJ_SCRIPT_OBJECT) {
		asIScriptObject *wrapper = wrap_object(p_object);
		if (wrapper == nullptr) {
			return nullptr;
		}
		asITypeInfo *wrapper_type = wrapper->GetObjectType();
		if (wrapper_type != p_target && !wrapper_type->DerivesFrom(p_target)) {
			wrapper->Release();
			return nullptr;
		}
		return wrapper; // strong reference, caller manages
	}
	return nullptr;
}

static void take_pending_thunk(asIScriptGeneric *p_gen) {
	p_gen->SetReturnObject(pending_owner);
	pending_owner = nullptr;
}

// Called from the generated GodotObject constructor with `this`. Binds the
// pending owner when one is queued (attach/wrap flows); otherwise creates a
// fresh native object of the most-derived generated class (`Node()` in
// scripts). Constructor chains run base-first, so only this root hook may
// instantiate -- per-class constructors would create the wrong type.
static void bind_self_thunk(asIScriptGeneric *p_gen) {
	if (pending_owner != nullptr) {
		p_gen->SetReturnObject(pending_owner);
		pending_owner = nullptr;
		return;
	}
	if (suppress_native_instantiate) {
		p_gen->SetReturnObject(nullptr);
		return;
	}
	int type_id = p_gen->GetArgTypeId(0);
	void *ref = p_gen->GetArgAddress(0);
	asIScriptObject *obj = nullptr;
	if (type_id & asTYPEID_OBJHANDLE) {
		obj = *static_cast<asIScriptObject **>(ref);
	} else {
		obj = static_cast<asIScriptObject *>(ref);
	}
	if (obj == nullptr) {
		p_gen->SetReturnObject(nullptr);
		return;
	}
	asITypeInfo *type = obj->GetObjectType();
	while (type != nullptr) {
		asIScriptModule *module = type->GetModule();
		if (module != nullptr && strcmp(module->GetName(), "GodotBases") == 0) {
			break;
		}
		type = type->GetBaseType();
	}
	if (type == nullptr) {
		p_gen->SetReturnObject(nullptr);
		return;
	}
	String cls = type->GetName();
	if (cls == "GodotObject") {
		cls = "Object";
	}
	Variant instance = ClassDBSingleton::get_singleton()->instantiate(StringName(cls));
	Object *native = instance.operator Object *();
	RefCounted *rc = Object::cast_to<RefCounted>(native);
	if (rc != nullptr && AsEnvironment::get_singleton() != nullptr) {
		AsEnvironment::get_singleton()->keep_alive(rc);
	}
	p_gen->SetReturnObject(native);
}

static void wrap_thunk(asIScriptGeneric *p_gen) {
	Object *obj = static_cast<Object *>(p_gen->GetArgObject(0));
	CScriptHandle *out = memnew_placement(p_gen->GetAddressOfReturnLocation(), CScriptHandle);
	if (obj == nullptr) {
		return;
	}
	asIScriptObject *wrapper = wrap_object(obj);
	if (wrapper == nullptr) {
		return;
	}
	out->Set(wrapper, wrapper->GetObjectType()); // Set() addrefs
	wrapper->Release();
}

void register_wrap_helpers(asIScriptEngine *p_engine) {
	RegisterScriptHandle(p_engine);
	p_engine->SetDefaultNamespace("godot");
	p_engine->RegisterGlobalFunction("Object@ __take_pending()",
			asFUNCTION(take_pending_thunk), asCALL_GENERIC);
	p_engine->RegisterGlobalFunction("Object@ __bind_self(const ?&in obj)",
			asFUNCTION(bind_self_thunk), asCALL_GENERIC);
	p_engine->RegisterGlobalFunction("ref __wrap(Object@ obj)",
			asFUNCTION(wrap_thunk), asCALL_GENERIC);
	p_engine->SetDefaultNamespace("");
}

// ---------------------------------------------------------------------------
// Source generation
// ---------------------------------------------------------------------------

// Script-space type for a PropertyInfo dict: like as_type_from_property but
// object types map to the generated script class (no godot:: prefix).
static String script_type_from_property(const Dictionary &p_info) {
	String native = as_type_from_property(p_info);
	if (native.begins_with("godot::")) {
		String cls = native.trim_prefix("godot::").trim_suffix("@");
		if (!get_class_specs().has(StringName(cls)) || singleton_classes.has(StringName(cls))) {
			cls = "Object";
		}
		return cls + "@";
	}
	return native;
}

static String method_script_return_type(const Dictionary &p_method) {
	Dictionary ret = p_method.get("return", Dictionary());
	Variant::Type type = Variant::Type(int(ret.get("type", 0)));
	int usage = ret.get("usage", 0);
	if (type == Variant::NIL) {
		return (usage & PROPERTY_USAGE_NIL_IS_VARIANT) ? String("Variant") : String();
	}
	return script_type_from_property(ret);
}

// Emits one forwarding method on a script base class. p_extra_variants adds
// trailing `const Variant &in` parameters for vararg engine methods.
static void emit_method_forward(String &r_out, const ClassSpec &p_spec, const Dictionary &p_method,
		bool p_as_property_accessor, int p_extra_variants) {
	String name = p_method.get("name", "");
	String as_name = sanitize_identifier(name);
	Array args = p_method.get("args", Array());
	Array default_args = p_method.get("default_args", Array());

	String ret_script = method_script_return_type(p_method);
	String ret_decl = ret_script.is_empty() ? String("void") : ret_script;

	// Parameters. Object parameters take script-space types and are unwrapped
	// in the body; defaults are dropped for parameters we cannot encode (the
	// native layer still applies engine defaults for trailing args, so we
	// generate overloads instead when defaults are unencodable -- v1 keeps it
	// simple and only encodes representable defaults).
	PackedStringArray params;
	PackedStringArray body_args;
	String pre_body;
	int argc = args.size();
	int first_default = argc - default_args.size();

	// Only the longest encodable trailing run of defaults may be emitted.
	LocalVector<String> literals;
	literals.resize(argc);
	int defaults_start = argc;
	if (p_extra_variants == 0) {
		for (int i = argc - 1; i >= first_default && i >= 0; i--) {
			Dictionary arg = args[i];
			String script_type = script_type_from_property(arg);
			String literal = variant_to_as_literal(default_args[i - first_default], script_type);
			if (literal.is_empty()) {
				break;
			}
			literals[i] = literal;
			defaults_start = i;
		}
	}

	for (int i = 0; i < argc; i++) {
		Dictionary arg = args[i];
		String arg_name = sanitize_identifier(arg.get("name", vformat("arg%d", i)));
		String script_type = script_type_from_property(arg);
		String native_type = as_type_from_property(arg);
		if (script_type.ends_with("@") && !native_type.begins_with("godot::")) {
			script_type = "Variant";
		}
		String param = script_type.ends_with("@")
				? script_type + " " + arg_name
				: (script_type == "bool" || script_type == "int64" || script_type == "double")
						? script_type + " " + arg_name
						: "const " + script_type + " &in " + arg_name;
		if (i >= defaults_start) {
			param += " = " + literals[i];
		}
		params.push_back(param);

		if (script_type.ends_with("@")) {
			// Unwrap script wrapper to the native parameter type.
			String tmp = "__p" + itos(i);
			pre_body += "\t\t" + native_type + " " + tmp + " = null;\n";
			pre_body += "\t\tif (" + arg_name + " !is null) { @" + tmp + " = cast<" + native_type.trim_suffix("@") + ">(" + arg_name + ".__self); }\n";
			body_args.push_back(tmp);
		} else {
			body_args.push_back(arg_name);
		}
	}
	for (int i = 0; i < p_extra_variants; i++) {
		params.push_back(vformat("const Variant &in vararg%d", i));
		body_args.push_back(vformat("vararg%d", i));
	}

	String call = "cast<godot::" + String(p_spec.name) + ">(__self)." + as_name + "(" + String(", ").join(body_args) + ")";

	String suffix = p_as_property_accessor ? String(" property") : String();
	r_out += "\t" + ret_decl + " " + as_name + "(" + String(", ").join(params) + ")" + suffix + " {\n";
	r_out += pre_body;
	if (ret_script.is_empty()) {
		r_out += "\t\t" + call + ";\n";
	} else if (ret_script.ends_with("@")) {
		r_out += "\t\treturn cast<" + ret_script.trim_suffix("@") + ">(godot::__wrap(" + call + "));\n";
	} else {
		r_out += "\t\treturn " + call + ";\n";
	}
	r_out += "\t}\n";
}

static void emit_class(String &r_out, const ClassSpec &p_spec) {
	String cls = p_spec.name;
	StringName parent_name = p_spec.parent;
	while (get_class_specs().has(parent_name) && singleton_classes.has(parent_name)) {
		parent_name = get_class_specs().get(parent_name).parent;
	}
	String parent = get_class_specs().has(parent_name) ? String(parent_name) : String("GodotObject");

	r_out += "shared class " + cls + " : " + parent + " {\n";


	// Property accessor decisions mirror native_classes.cpp.
	HashMap<String, String> getter_to_prop;
	HashMap<String, String> setter_to_prop;
	HashSet<String> method_names;
	for (int i = 0; i < p_spec.methods.size(); i++) {
		Dictionary m = p_spec.methods[i];
		method_names.insert(m.get("name", ""));
	}
	for (int i = 0; i < p_spec.properties.size(); i++) {
		Dictionary prop = p_spec.properties[i];
		String prop_name = prop.get("name", "");
		if (prop_name.is_empty() || !prop_name.is_valid_ascii_identifier()) {
			continue;
		}
		String getter = ClassDBSingleton::get_singleton()->class_get_property_getter(p_spec.name, prop_name);
		String setter = ClassDBSingleton::get_singleton()->class_get_property_setter(p_spec.name, prop_name);
		if (!getter.is_empty()) {
			getter_to_prop.insert(getter, prop_name);
		}
		if (!setter.is_empty()) {
			setter_to_prop.insert(setter, prop_name);
		}

		// Accessors for properties whose getter is not get_<prop>. The native
		// layer registered matching get_/set_ accessor functions; call them
		// directly to avoid handle-property assignment semantics.
		if (getter != "get_" + prop_name && !method_names.has("get_" + prop_name)) {
			String script_type = script_type_from_property(prop);
			String native_call = "cast<godot::" + cls + ">(__self).get_" + prop_name + "()";
			r_out += "\t" + script_type + " get_" + prop_name + "() property {\n";
			if (script_type.ends_with("@")) {
				r_out += "\t\treturn cast<" + script_type.trim_suffix("@") + ">(godot::__wrap(" + native_call + "));\n";
			} else {
				r_out += "\t\treturn " + native_call + ";\n";
			}
			r_out += "\t}\n";
			if (!setter.is_empty() && !method_names.has("set_" + prop_name)) {
				if (script_type.ends_with("@")) {
					String native_type = as_type_from_property(prop);
					r_out += "\tvoid set_" + prop_name + "(" + script_type + " value) property {\n";
					r_out += "\t\t" + native_type + " __v = null;\n";
					r_out += "\t\tif (value !is null) { @__v = cast<" + native_type.trim_suffix("@") + ">(value.__self); }\n";
					r_out += "\t\tcast<godot::" + cls + ">(__self).set_" + prop_name + "(__v);\n";
					r_out += "\t}\n";
				} else {
					String param = (script_type == "bool" || script_type == "int64" || script_type == "double")
							? script_type
							: "const " + script_type + " &in";
					r_out += "\tvoid set_" + prop_name + "(" + param + " value) property {\n";
					r_out += "\t\tcast<godot::" + cls + ">(__self).set_" + prop_name + "(value);\n";
					r_out += "\t}\n";
				}
			}
		}
	}

	// Method forwards.
	for (int i = 0; i < p_spec.methods.size(); i++) {
		Dictionary m = p_spec.methods[i];
		String name = m.get("name", "");
		int flags = m.get("flags", 0);
		if ((flags & METHOD_FLAG_VIRTUAL) || (flags & METHOD_FLAG_STATIC)) {
			continue;
		}
		if (name.is_empty() || !name.is_valid_ascii_identifier()) {
			continue;
		}
		Array args = m.get("args", Array());
		String ret_type_name = method_script_return_type(m);
		bool accessor = false;
		if (getter_to_prop.has(name) && name == "get_" + getter_to_prop.get(name) && args.size() == 0 && !ret_type_name.is_empty()) {
			accessor = true;
		} else if (setter_to_prop.has(name) && name == "set_" + setter_to_prop.get(name) && args.size() == 1 && ret_type_name.is_empty()) {
			accessor = true;
		}
		bool is_vararg = flags & METHOD_FLAG_VARARG;
		int extras_max = is_vararg ? 8 : 0;
		for (int extra = 0; extra <= extras_max; extra++) {
			emit_method_forward(r_out, p_spec, m, accessor && extra == 0, extra);
		}
	}

	// Signal accessors.
	for (int i = 0; i < p_spec.signals.size(); i++) {
		Dictionary sig = p_spec.signals[i];
		String name = sig.get("name", "");
		if (name.is_empty() || !name.is_valid_ascii_identifier() || method_names.has("get_" + name)) {
			continue;
		}
		r_out += "\tSignal get_" + name + "() property { return godot::__signal(__self, StringName(\"" + name + "\")); }\n";
	}

	r_out += "}\n\n";
}

static String generate_bases_source() {
	singleton_classes.clear();
	for (const String &name : Engine::get_singleton()->get_singleton_list()) {
		StringName cls_name(name);
		if (get_class_specs().has(cls_name)) {
			singleton_classes.insert(cls_name);
		}
	}

	String out;
	out += "// Generated script-space mirror of ClassDB. Do not edit.\n";
	out += "shared class GodotObject {\n";
	out += "\tgodot::Object@ __self;\n";
	out += "\tGodotObject() { @__self = godot::__bind_self(this); }\n";
	out += "\tgodot::Object@ get_native() property { return __self; }\n";
	out += "\tbool opEquals(const GodotObject@ other) const {\n";
	out += "\t\tif (other is null) { return __self is null; }\n";
	out += "\t\treturn __self is other.__self;\n";
	out += "\t}\n";
	out += "\tVariant opImplConv() const { return godot::__to_variant(__self); }\n";
	out += "}\n\n";

	for (const KeyValue<StringName, ClassSpec> &kv : get_class_specs()) {
		if (singleton_classes.has(kv.key)) {
			continue; // name is reserved for the singleton accessor
		}
		emit_class(out, kv.value);
	}
	return out;
}

bool build_bases_module(asIScriptEngine *p_engine) {
	uint64_t start = Time::get_singleton() != nullptr ? Time::get_singleton()->get_ticks_msec() : 0;
	String source = generate_bases_source();

	String dump_path = OS::get_singleton()->get_environment("GDAS_DUMP_BASES");
	if (!dump_path.is_empty()) {
		Ref<FileAccess> dump = FileAccess::open(dump_path, FileAccess::WRITE);
		if (dump.is_valid()) {
			dump->store_string(source);
		}
	}

	asIScriptModule *module = p_engine->GetModule("GodotBases", asGM_ALWAYS_CREATE);
	CharString utf8 = source.utf8();
	module->AddScriptSection("GodotBases.gen.as", utf8.get_data(), utf8.length());
	int r = module->Build();
	if (r < 0) {
		ERR_PRINT("AngelScript: failed to build the GodotBases module.");
		return false;
	}
	if (Time::get_singleton() != nullptr) {
		UtilityFunctions::print_verbose(vformat("AngelScript: GodotBases compiled in %d ms (%d classes).",
				Time::get_singleton()->get_ticks_msec() - start, int64_t(get_class_specs().size())));
	}
	return true;
}

String get_user_module_header() {
	if (cached_user_header != nullptr) {
		return *cached_user_header;
	}
	String out;
	out += "external shared class GodotObject;\n";
	for (const KeyValue<StringName, ClassSpec> &kv : get_class_specs()) {
		if (singleton_classes.has(kv.key)) {
			continue;
		}
		out += "external shared class " + String(kv.key) + ";\n";
	}

	// Singleton accessors (Input, Engine, ...) return native handles; the
	// singleton classes themselves are excluded from script-space generation
	// so these names are free.
	PackedStringArray singletons = Engine::get_singleton()->get_singleton_list();
	for (const String &name : singletons) {
		if (!name.is_valid_ascii_identifier()) {
			continue;
		}
		Object *obj = Engine::get_singleton()->get_singleton(name);
		if (obj == nullptr) {
			continue;
		}
		StringName cls = obj->get_class();
		if (!get_class_specs().has(cls)) {
			continue;
		}
		out += "godot::" + String(cls) + "@ get_" + name + "() property { return cast<godot::" + String(cls) +
				">(godot::__singleton(StringName(\"" + name + "\"))); }\n";
	}
	cached_user_header = memnew(String(out));
	cached_user_header_lines = out.count("\n");
	return out;
}

int get_user_module_header_line_count() {
	if (cached_user_header == nullptr) {
		get_user_module_header();
	}
	return cached_user_header_lines;
}

} // namespace gdas
