#include "native_classes.h"

#include "as_environment.h"
#include "builtin_types.h"
#include "script_bases.h"
#include "variant_bridge.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

#define AS_CHECK(expr)                                                              \
	do {                                                                            \
		int _r = (expr);                                                            \
		if (_r < 0) {                                                               \
			ERR_PRINT(vformat("AngelScript native registration failed (%d): %s", _r, #expr)); \
		}                                                                           \
	} while (0)

static HashMap<StringName, ClassSpec> class_specs;

const HashMap<StringName, ClassSpec> &get_class_specs() {
	return class_specs;
}

bool is_native_class(const StringName &p_name) {
	return class_specs.has(p_name);
}

// ---------------------------------------------------------------------------
// Auxiliary bind data
// ---------------------------------------------------------------------------

struct NativeMethodBind {
	StringName method;
	StringName cls;
	bool is_static = false;
	bool returns_object = false;
};

struct NativeAccessorBind {
	StringName property;
	bool setter = false;
};

struct SignalAccessorBind {
	StringName signal;
};

static LocalVector<NativeMethodBind *> native_method_binds;
static LocalVector<NativeAccessorBind *> native_accessor_binds;
static LocalVector<SignalAccessorBind *> signal_accessor_binds;
static LocalVector<NativeClassInfo *> native_class_infos;
static LocalVector<StringName *> cast_targets;
static LocalVector<int64_t *> constant_storage;

void native_classes_free_bind_data() {
	for (auto *p : native_method_binds) { memdelete(p); }
	for (auto *p : native_accessor_binds) { memdelete(p); }
	for (auto *p : signal_accessor_binds) { memdelete(p); }
	for (auto *p : native_class_infos) { memdelete(p); }
	for (auto *p : cast_targets) { memdelete(p); }
	for (auto *p : constant_storage) { memfree(p); }
	native_method_binds.clear();
	native_accessor_binds.clear();
	signal_accessor_binds.clear();
	native_class_infos.clear();
	cast_targets.clear();
	constant_storage.clear();
	class_specs.clear();
}

// ---------------------------------------------------------------------------
// Thunks
// ---------------------------------------------------------------------------

static void keep_alive_if_refcounted(const Variant &p_value) {
	if (p_value.get_type() != Variant::OBJECT) {
		return;
	}
	Object *obj = p_value.operator Object *();
	RefCounted *rc = Object::cast_to<RefCounted>(obj);
	if (rc != nullptr && AsEnvironment::get_singleton() != nullptr) {
		AsEnvironment::get_singleton()->keep_alive(rc);
	}
}

static void native_method_thunk(asIScriptGeneric *p_gen) {
	NativeMethodBind *bind = static_cast<NativeMethodBind *>(p_gen->GetAuxiliary());
	int argc = int(p_gen->GetArgCount());

	Array args;
	args.resize(argc);
	for (int i = 0; i < argc; i++) {
		args[i] = generic_arg_to_variant(p_gen, i);
	}

	Variant ret;
	if (bind->is_static) {
		Array call_args;
		call_args.push_back(bind->cls);
		call_args.push_back(bind->method);
		call_args.append_array(args);
		ret = ClassDBSingleton::get_singleton()->callv("class_call_static", call_args);
	} else {
		Object *obj = static_cast<Object *>(p_gen->GetObject());
		if (obj == nullptr) {
			asIScriptContext *ctx = asGetActiveContext();
			if (ctx != nullptr) {
				ctx->SetException("Null object in native method call");
			}
			return;
		}
		ret = obj->callv(bind->method, args);
	}
	if (bind->returns_object) {
		keep_alive_if_refcounted(ret);
	}
	generic_set_return(p_gen, ret);
}

static void native_accessor_thunk(asIScriptGeneric *p_gen) {
	NativeAccessorBind *bind = static_cast<NativeAccessorBind *>(p_gen->GetAuxiliary());
	Object *obj = static_cast<Object *>(p_gen->GetObject());
	if (obj == nullptr) {
		return;
	}
	if (bind->setter) {
		obj->set(bind->property, generic_arg_to_variant(p_gen, 0));
	} else {
		Variant value = obj->get(bind->property);
		keep_alive_if_refcounted(value);
		generic_set_return(p_gen, value);
	}
}

static void signal_accessor_thunk(asIScriptGeneric *p_gen) {
	SignalAccessorBind *bind = static_cast<SignalAccessorBind *>(p_gen->GetAuxiliary());
	Object *obj = static_cast<Object *>(p_gen->GetObject());
	memnew_placement(p_gen->GetAddressOfReturnLocation(), Signal(obj, bind->signal));
}

static void upcast_thunk(asIScriptGeneric *p_gen) {
	// Godot single-inheritance objects share one pointer; upcasts are identity.
	p_gen->SetReturnObject(p_gen->GetObject());
}

static void downcast_thunk(asIScriptGeneric *p_gen) {
	StringName *target = static_cast<StringName *>(p_gen->GetAuxiliary());
	Object *obj = static_cast<Object *>(p_gen->GetObject());
	if (obj != nullptr && obj->is_class(*target)) {
		p_gen->SetReturnObject(obj);
	} else {
		p_gen->SetReturnObject(nullptr);
	}
}

static void factory_thunk(asIScriptGeneric *p_gen) {
	StringName *cls = static_cast<StringName *>(p_gen->GetAuxiliary());
	Variant instance = ClassDBSingleton::get_singleton()->instantiate(*cls);
	keep_alive_if_refcounted(instance);
	p_gen->SetReturnObject(instance.operator Object *());
}

static void singleton_thunk(asIScriptGeneric *p_gen) {
	StringName name = *static_cast<StringName *>(p_gen->GetArgObject(0));
	p_gen->SetReturnObject(Engine::get_singleton()->get_singleton(name));
}

static void instantiate_thunk(asIScriptGeneric *p_gen) {
	StringName name = *static_cast<StringName *>(p_gen->GetArgObject(0));
	Variant instance = ClassDBSingleton::get_singleton()->instantiate(name);
	keep_alive_if_refcounted(instance);
	p_gen->SetReturnObject(instance.operator Object *());
}

static void make_signal_thunk(asIScriptGeneric *p_gen) {
	Object *obj = static_cast<Object *>(p_gen->GetArgObject(0));
	StringName name = *static_cast<StringName *>(p_gen->GetArgObject(1));
	memnew_placement(p_gen->GetAddressOfReturnLocation(), Signal(obj, name));
}

static void to_variant_thunk(asIScriptGeneric *p_gen) {
	Object *obj = static_cast<Object *>(p_gen->GetArgObject(0));
	memnew_placement(p_gen->GetAddressOfReturnLocation(), Variant(obj));
}

static void instance_from_id_thunk(asIScriptGeneric *p_gen) {
	int64_t id = int64_t(p_gen->GetArgQWord(0));
	Object *obj = ObjectDB::get_instance(ObjectID(uint64_t(id)));
	p_gen->SetReturnObject(obj);
}

// ---------------------------------------------------------------------------
// Declaration helpers
// ---------------------------------------------------------------------------

// Best-effort encoding of a default argument Variant into an AngelScript
// literal. Empty return means "cannot encode".
String variant_to_as_literal(const Variant &p_value, const String &p_param_type) {
	switch (p_value.get_type()) {
		case Variant::NIL:
			return p_param_type.ends_with("@") ? String("null") : String("Variant()");
		case Variant::BOOL:
			return p_value.operator bool() ? "true" : "false";
		case Variant::INT:
			return String::num_int64(p_value.operator int64_t());
		case Variant::FLOAT: {
			double d = p_value.operator double();
			String s = String::num(d, 10);
			if (!s.contains(".") && !s.contains("e") && !s.contains("inf") && !s.contains("nan")) {
				s += ".0";
			}
			if (s.contains("inf") || s.contains("nan")) {
				return String();
			}
			return s;
		}
		case Variant::STRING: {
			String s = p_value.operator String();
			return "\"" + s.c_escape() + "\"";
		}
		case Variant::STRING_NAME:
			return "StringName(\"" + String(p_value.operator StringName()).c_escape() + "\")";
		case Variant::NODE_PATH:
			return "NodePath(\"" + String(p_value.operator NodePath()).c_escape() + "\")";
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
		case Variant::VECTOR4:
		case Variant::VECTOR4I: {
			String repr = p_value.stringify(); // e.g. "(0, 0)"
			static const char *names[] = { "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i" };
			int idx = 0;
			switch (p_value.get_type()) {
				case Variant::VECTOR2: idx = 0; break;
				case Variant::VECTOR2I: idx = 1; break;
				case Variant::VECTOR3: idx = 2; break;
				case Variant::VECTOR3I: idx = 3; break;
				case Variant::VECTOR4: idx = 4; break;
				default: idx = 5; break;
			}
			return String(names[idx]) + repr;
		}
		case Variant::COLOR: {
			Color c = p_value.operator Color();
			return vformat("Color(%f, %f, %f, %f)", c.r, c.g, c.b, c.a);
		}
		case Variant::ARRAY:
			return p_value.operator Array().is_empty() ? String("Array()") : String();
		case Variant::DICTIONARY:
			return p_value.operator Dictionary().is_empty() ? String("Dictionary()") : String();
		case Variant::CALLABLE:
			return p_value.operator Callable().is_null() ? String("Callable()") : String();
		case Variant::RID:
			return "RID()";
		case Variant::OBJECT:
			return p_value.operator Object *() == nullptr ? String("null") : String();
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY: {
			if (p_value.booleanize()) {
				return String();
			}
			return p_param_type + String("()");
		}
		default:
			return String();
	}
}

// Returns the return type for a MethodInfo dict, "" for void.
static String method_return_type(const Dictionary &p_method) {
	Dictionary ret = p_method.get("return", Dictionary());
	Variant::Type type = Variant::Type(int(ret.get("type", 0)));
	int usage = ret.get("usage", 0);
	if (type == Variant::NIL) {
		if (usage & PROPERTY_USAGE_NIL_IS_VARIANT) {
			return "Variant";
		}
		return "";
	}
	return as_type_from_property(ret);
}

// Builds a full AngelScript declaration for a reflected method.
// r_fixed_argc receives the number of declared (non-vararg) parameters.
static String build_method_decl(const Dictionary &p_method, const String &p_as_name,
		int p_extra_variants, bool p_is_static) {
	Array args = p_method.get("args", Array());
	Array default_args = p_method.get("default_args", Array());
	String ret = method_return_type(p_method);
	if (ret.is_empty()) {
		ret = "void";
	}

	int argc = args.size();
	int first_default = argc - default_args.size();

	// AngelScript requires every parameter after the first default to carry a
	// default, so only the longest encodable trailing run may keep them.
	LocalVector<String> literals;
	literals.resize(argc);
	int defaults_start = argc;
	for (int i = argc - 1; i >= first_default; i--) {
		Dictionary arg = args[i];
		String literal = variant_to_as_literal(default_args[i - first_default], as_type_from_property(arg));
		if (literal.is_empty()) {
			break;
		}
		literals[i] = literal;
		defaults_start = i;
	}

	PackedStringArray parts;
	for (int i = 0; i < argc; i++) {
		Dictionary arg = args[i];
		String as_type = as_type_from_property(arg);
		String part = as_param_decl(as_type) + " " + sanitize_identifier(arg.get("name", vformat("arg%d", i)));
		if (i >= defaults_start) {
			part += " = " + literals[i];
		}
		parts.push_back(part);
	}
	for (int i = 0; i < p_extra_variants; i++) {
		parts.push_back(vformat("const Variant &in vararg%d", i));
	}
	String decl = ret + " " + p_as_name + "(" + String(", ").join(parts) + ")";
	if (!p_is_static) {
		decl += " const";
	}
	return decl;
}

static bool is_valid_identifier(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_name.length(); i++) {
		char32_t c = p_name[i];
		bool ok = (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && c >= '0' && c <= '9');
		if (!ok) {
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_native_classes(asIScriptEngine *p_engine) {
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	PackedStringArray classes = class_db->get_class_list();

	AS_CHECK(p_engine->SetDefaultNamespace("godot"));

	// Phase A: type shells, so any signature can reference any class.
	for (const String &cls : classes) {
		if (!is_valid_identifier(cls)) {
			continue;
		}
		int r = p_engine->RegisterObjectType(cls.utf8().get_data(), 0, asOBJ_REF | asOBJ_NOCOUNT);
		if (r < 0) {
			WARN_PRINT(vformat("AngelScript: could not register native class %s (%d).", cls, r));
			continue;
		}
		ClassSpec spec;
		spec.name = StringName(cls);
		spec.parent = class_db->get_parent_class(cls);
		spec.refcounted = class_db->is_parent_class(cls, "RefCounted");
		spec.instantiable = class_db->can_instantiate(cls);
		class_specs.insert(spec.name, spec);

		NativeClassInfo *info = memnew(NativeClassInfo);
		info->godot_class = spec.name;
		info->refcounted = spec.refcounted;
		native_class_infos.push_back(info);
		asITypeInfo *type = p_engine->GetTypeInfoByName(cls.utf8().get_data());
		type->SetUserData(info, NATIVE_CLASS_USERDATA);
	}

	// Builtin members may reference godot::Object etc.
	AS_CHECK(p_engine->SetDefaultNamespace(""));
	register_builtin_members(p_engine);
	AS_CHECK(p_engine->SetDefaultNamespace("godot"));

	// Phase B: members.
	for (KeyValue<StringName, ClassSpec> &kv : class_specs) {
		ClassSpec &spec = kv.value;
		String cls = spec.name;
		CharString cls_utf8 = cls.utf8();

		// Implicit upcasts along the ancestor chain, explicit downcasts back.
		StringName parent = spec.parent;
		while (parent != StringName() && class_specs.has(parent)) {
			String parent_str = parent;
			AS_CHECK(p_engine->RegisterObjectMethod(cls_utf8.get_data(),
					(parent_str + "@ opImplCast() const").utf8().get_data(),
					asFUNCTION(upcast_thunk), asCALL_GENERIC));
			StringName *target = memnew(StringName(spec.name));
			cast_targets.push_back(target);
			AS_CHECK(p_engine->RegisterObjectMethod(parent_str.utf8().get_data(),
					(cls + "@ opCast() const").utf8().get_data(),
					asFUNCTION(downcast_thunk), asCALL_GENERIC, target));
			parent = class_specs.get(parent).parent;
		}

		// Factory for instantiable classes: `godot::Node()`.
		if (spec.instantiable) {
			StringName *target = memnew(StringName(spec.name));
			cast_targets.push_back(target);
			AS_CHECK(p_engine->RegisterObjectBehaviour(cls_utf8.get_data(), asBEHAVE_FACTORY,
					(cls + "@ f()").utf8().get_data(),
					asFUNCTION(factory_thunk), asCALL_GENERIC, target));
		}

		// Property accessor bookkeeping.
		spec.properties = class_db->class_get_property_list(spec.name, true);
		HashMap<String, String> getter_to_prop;
		HashMap<String, String> setter_to_prop;
		HashSet<String> method_names;
		spec.methods = class_db->class_get_method_list(spec.name, true);
		for (int i = 0; i < spec.methods.size(); i++) {
			Dictionary m = spec.methods[i];
			method_names.insert(m.get("name", ""));
		}
		for (int i = 0; i < spec.properties.size(); i++) {
			Dictionary prop = spec.properties[i];
			String prop_name = prop.get("name", "");
			if (!is_valid_identifier(prop_name)) {
				continue;
			}
			String getter = class_db->class_get_property_getter(spec.name, prop_name);
			String setter = class_db->class_get_property_setter(spec.name, prop_name);
			if (!getter.is_empty()) {
				getter_to_prop.insert(getter, prop_name);
			}
			if (!setter.is_empty()) {
				setter_to_prop.insert(setter, prop_name);
			}

			// When the getter doesn't follow get_<prop> naming (e.g.
			// is_visible), add an explicit accessor so `obj.visible` works.
			String expected_getter = "get_" + prop_name;
			if (getter != expected_getter && !method_names.has(expected_getter)) {
				String as_type = as_type_from_property(prop);
				NativeAccessorBind *get_bind = memnew(NativeAccessorBind);
				get_bind->property = StringName(prop_name);
				native_accessor_binds.push_back(get_bind);
				AS_CHECK(p_engine->RegisterObjectMethod(cls_utf8.get_data(),
						(as_type + " get_" + prop_name + "() const property").utf8().get_data(),
						asFUNCTION(native_accessor_thunk), asCALL_GENERIC, get_bind));
				if (!setter.is_empty() && !method_names.has("set_" + prop_name)) {
					NativeAccessorBind *set_bind = memnew(NativeAccessorBind);
					set_bind->property = StringName(prop_name);
					set_bind->setter = true;
					native_accessor_binds.push_back(set_bind);
					AS_CHECK(p_engine->RegisterObjectMethod(cls_utf8.get_data(),
							(String("void set_") + prop_name + "(" + as_param_decl(as_type) + " value) property").utf8().get_data(),
							asFUNCTION(native_accessor_thunk), asCALL_GENERIC, set_bind));
				}
			}
		}

		// Methods.
		for (int i = 0; i < spec.methods.size(); i++) {
			Dictionary m = spec.methods[i];
			String name = m.get("name", "");
			int flags = m.get("flags", 0);
			bool is_static = flags & METHOD_FLAG_STATIC;
			bool is_vararg = flags & METHOD_FLAG_VARARG;
			bool is_virtual = flags & METHOD_FLAG_VIRTUAL;
			if (is_virtual || !is_valid_identifier(name)) {
				continue; // virtuals are overridden in scripts, never called on natives
			}
			String as_name = sanitize_identifier(name);

			// Mark plain get_x/set_x methods backing a property as accessors.
			String property_suffix;
			Array args = m.get("args", Array());
			String ret_type_name = method_return_type(m);
			if (getter_to_prop.has(name) && name == "get_" + getter_to_prop.get(name) && args.size() == 0 && !is_static && !ret_type_name.is_empty()) {
				property_suffix = " property";
			} else if (setter_to_prop.has(name) && name == "set_" + setter_to_prop.get(name) && args.size() == 1 && !is_static && ret_type_name.is_empty()) {
				property_suffix = " property";
			}

			NativeMethodBind *bind = memnew(NativeMethodBind);
			bind->method = StringName(name);
			bind->cls = spec.name;
			bind->is_static = is_static;
			bind->returns_object = method_return_type(m).begins_with("godot::") || method_return_type(m) == "Variant";
			native_method_binds.push_back(bind);

			int extras_max = is_vararg ? 8 : 0;
			for (int extra = 0; extra <= extras_max; extra++) {
				String decl = build_method_decl(m, as_name, extra, is_static) + property_suffix;
				if (is_static) {
					// Static methods: godot::ClassName_method(...) global.
					String static_decl = build_method_decl(m, cls + "_" + as_name, extra, true);
					AS_CHECK(p_engine->RegisterGlobalFunction(static_decl.utf8().get_data(),
							asFUNCTION(native_method_thunk), asCALL_GENERIC, bind));
				} else {
					AS_CHECK(p_engine->RegisterObjectMethod(cls_utf8.get_data(), decl.utf8().get_data(),
							asFUNCTION(native_method_thunk), asCALL_GENERIC, bind));
				}
			}
		}

		// Signals become read-only `Signal` properties.
		spec.signals = class_db->class_get_signal_list(spec.name, true);
		for (int i = 0; i < spec.signals.size(); i++) {
			Dictionary sig = spec.signals[i];
			String name = sig.get("name", "");
			if (!is_valid_identifier(name) || method_names.has("get_" + name)) {
				continue;
			}
			SignalAccessorBind *bind = memnew(SignalAccessorBind);
			bind->signal = StringName(name);
			signal_accessor_binds.push_back(bind);
			AS_CHECK(p_engine->RegisterObjectMethod(cls_utf8.get_data(),
					(String("Signal get_") + name + "() const property").utf8().get_data(),
					asFUNCTION(signal_accessor_thunk), asCALL_GENERIC, bind));
		}

		// Enums and integer constants: godot::ClassName_EnumName /
		// godot::ClassName_CONSTANT.
		PackedStringArray enum_list = class_db->class_get_enum_list(spec.name, true);
		HashSet<String> enum_constants;
		for (const String &enum_name : enum_list) {
			if (!is_valid_identifier(enum_name)) {
				continue;
			}
			String as_enum = cls + "_" + enum_name;
			if (p_engine->RegisterEnum(as_enum.utf8().get_data()) < 0) {
				continue;
			}
			PackedStringArray values = class_db->class_get_enum_constants(spec.name, enum_name, true);
			for (const String &value_name : values) {
				enum_constants.insert(value_name);
				int64_t value = class_db->class_get_integer_constant(spec.name, value_name);
				if (value >= INT32_MIN && value <= INT32_MAX && is_valid_identifier(value_name)) {
					AS_CHECK(p_engine->RegisterEnumValue(as_enum.utf8().get_data(), value_name.utf8().get_data(), int(value)));
				}
			}
		}
		PackedStringArray constants = class_db->class_get_integer_constant_list(spec.name, true);
		for (const String &const_name : constants) {
			if (enum_constants.has(const_name) || !is_valid_identifier(const_name)) {
				continue;
			}
			int64_t *storage = static_cast<int64_t *>(memalloc(sizeof(int64_t)));
			*storage = class_db->class_get_integer_constant(spec.name, const_name);
			constant_storage.push_back(storage);
			AS_CHECK(p_engine->RegisterGlobalProperty(
					(String("const int64 ") + cls + "_" + const_name).utf8().get_data(), storage));
		}
	}

	// Helper globals used by generated script bases.
	AS_CHECK(p_engine->RegisterGlobalFunction("Object@ __singleton(const StringName &in name)",
			asFUNCTION(singleton_thunk), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterGlobalFunction("Object@ __instantiate(const StringName &in cls)",
			asFUNCTION(instantiate_thunk), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterGlobalFunction("Signal __signal(Object@ obj, const StringName &in name)",
			asFUNCTION(make_signal_thunk), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterGlobalFunction("Variant __to_variant(Object@ obj)",
			asFUNCTION(to_variant_thunk), asCALL_GENERIC));

	AS_CHECK(p_engine->SetDefaultNamespace(""));
	AS_CHECK(p_engine->RegisterGlobalFunction("godot::Object@ instance_from_id(int64 id)",
			asFUNCTION(instance_from_id_thunk), asCALL_GENERIC));

	// The `ref` handle type + __wrap live in script_bases.
	register_wrap_helpers(p_engine);
}

} // namespace gdas
