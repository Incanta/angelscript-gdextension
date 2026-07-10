#include "builtin_types.h"

#include "builtin_bindings.gen.h"
#include "variant_bridge.h"

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string.h>

using namespace godot;

namespace gdas {

#define AS_CHECK(expr)                                                              \
	do {                                                                            \
		int _r = (expr);                                                            \
		if (_r < 0) {                                                               \
			ERR_PRINT(vformat("AngelScript registration failed (%d): %s", _r, #expr)); \
		}                                                                           \
	} while (0)

// ---------------------------------------------------------------------------
// String factory
// ---------------------------------------------------------------------------

class GodotStringFactory : public asIStringFactory {
	struct CachedString {
		String *str;
		int refcount;
	};
	HashMap<uint64_t, CachedString> cache;

	static uint64_t key_for(const char *p_data, asUINT p_length) {
		uint64_t h = 14695981039346656037ULL;
		for (asUINT i = 0; i < p_length; i++) {
			h = (h ^ uint8_t(p_data[i])) * 1099511628211ULL;
		}
		return h ^ p_length;
	}

public:
	const void *GetStringConstant(const char *p_data, asUINT p_length) override {
		uint64_t key = key_for(p_data, p_length);
		CachedString *found = cache.getptr(key);
		if (found != nullptr) {
			found->refcount++;
			return found->str;
		}
		String *str = memnew(String(String::utf8(p_data, p_length)));
		cache.insert(key, { str, 1 });
		return str;
	}

	int ReleaseStringConstant(const void *p_str) override {
		for (KeyValue<uint64_t, CachedString> &kv : cache) {
			if (kv.value.str == p_str) {
				kv.value.refcount--;
				if (kv.value.refcount <= 0) {
					memdelete(kv.value.str);
					cache.erase(kv.key);
				}
				return asSUCCESS;
			}
		}
		return asERROR;
	}

	int GetRawStringData(const void *p_str, char *p_data, asUINT *p_length) const override {
		const String *str = static_cast<const String *>(p_str);
		CharString utf8 = str->utf8();
		if (p_length != nullptr) {
			*p_length = asUINT(utf8.length());
		}
		if (p_data != nullptr) {
			memcpy(p_data, utf8.get_data(), utf8.length());
		}
		return asSUCCESS;
	}
};

static GodotStringFactory *string_factory = nullptr;

// ---------------------------------------------------------------------------
// Value type lifecycle thunks (auxiliary = Variant::Type as pointer int)
// ---------------------------------------------------------------------------

static Variant::Type aux_type(asIScriptGeneric *p_gen) {
	return Variant::Type(uintptr_t(p_gen->GetAuxiliary()));
}

static void value_construct(asIScriptGeneric *p_gen) {
	builtin_construct(aux_type(p_gen), p_gen->GetObject());
}

static void value_copy_construct(asIScriptGeneric *p_gen) {
	builtin_copy_construct(aux_type(p_gen), p_gen->GetObject(), p_gen->GetArgObject(0));
}

static void value_destruct(asIScriptGeneric *p_gen) {
	builtin_destruct(aux_type(p_gen), p_gen->GetObject());
}

static void value_assign(asIScriptGeneric *p_gen) {
	builtin_assign(aux_type(p_gen), p_gen->GetObject(), p_gen->GetArgObject(0));
	p_gen->SetReturnAddress(p_gen->GetObject());
}

// ---------------------------------------------------------------------------
// Variant registered type
// ---------------------------------------------------------------------------

static void variant_construct_default(asIScriptGeneric *p_gen) {
	memnew_placement(p_gen->GetObject(), Variant);
}

static void variant_construct_copyish(asIScriptGeneric *p_gen) {
	// Works for Variant itself and for any convertible argument.
	memnew_placement(p_gen->GetObject(), Variant(generic_arg_to_variant(p_gen, 0)));
}

static void variant_destruct(asIScriptGeneric *p_gen) {
	static_cast<Variant *>(p_gen->GetObject())->~Variant();
}

static void variant_assign_any(asIScriptGeneric *p_gen) {
	*static_cast<Variant *>(p_gen->GetObject()) = generic_arg_to_variant(p_gen, 0);
	p_gen->SetReturnAddress(p_gen->GetObject());
}

static void variant_opconv(asIScriptGeneric *p_gen) {
	generic_set_return(p_gen, *static_cast<Variant *>(p_gen->GetObject()));
}

static void variant_opequals(asIScriptGeneric *p_gen) {
	Variant &self = *static_cast<Variant *>(p_gen->GetObject());
	p_gen->SetReturnByte(self == generic_arg_to_variant(p_gen, 0) ? 1 : 0);
}

static void variant_get_type(asIScriptGeneric *p_gen) {
	p_gen->SetReturnDWord(asDWORD(static_cast<Variant *>(p_gen->GetObject())->get_type()));
}

static void variant_to_string(asIScriptGeneric *p_gen) {
	Variant &self = *static_cast<Variant *>(p_gen->GetObject());
	memnew_placement(p_gen->GetAddressOfReturnLocation(), String(self.stringify()));
}

// ---------------------------------------------------------------------------
// Generated table dispatch
// ---------------------------------------------------------------------------

struct MethodBindData {
	StringName method;
	Variant::Type cls_type;
	const gen::MethodSpec *spec;
};

struct MemberBindData {
	StringName member;
	Variant::Type cls_type;
	bool setter;
};

struct OperatorBindData {
	Variant::Operator op;
	bool reversed;
	bool synth_cmp; // synthesize opCmp from OP_LESS
};

struct CtorBindData {
	Variant::Type cls_type;
	const gen::CtorSpec *spec;
};

struct UtilityBindData {
	StringName name;
	GDExtensionPtrUtilityFunction fn = nullptr;
	const gen::UtilitySpec *spec = nullptr;
	Variant::Type ret_value_type = Variant::VARIANT_MAX;
	int ret_kind = 0; // 0 void, 1 primitive-int, 2 primitive-float, 3 primitive-bool, 4 value type, 5 Variant
};

// Keep auxiliary structures alive for the lifetime of the engine.
static LocalVector<MethodBindData *> method_binds;
static LocalVector<MemberBindData *> member_binds;
static LocalVector<OperatorBindData *> operator_binds;
static LocalVector<CtorBindData *> ctor_binds;
static LocalVector<UtilityBindData *> utility_binds;
static LocalVector<int64_t *> big_constants;

void builtin_types_free_bind_data() {
	for (MethodBindData *p : method_binds) { memdelete(p); }
	for (MemberBindData *p : member_binds) { memdelete(p); }
	for (OperatorBindData *p : operator_binds) { memdelete(p); }
	for (CtorBindData *p : ctor_binds) { memdelete(p); }
	for (UtilityBindData *p : utility_binds) { memdelete(p); }
	for (int64_t *p : big_constants) { memfree(p); }
	method_binds.clear();
	member_binds.clear();
	operator_binds.clear();
	ctor_binds.clear();
	utility_binds.clear();
	big_constants.clear();
	if (string_factory != nullptr) {
		memdelete(string_factory);
		string_factory = nullptr;
	}
}

static void builtin_method_thunk(asIScriptGeneric *p_gen) {
	MethodBindData *bind = static_cast<MethodBindData *>(p_gen->GetAuxiliary());
	int argc = int(p_gen->GetArgCount());

	LocalVector<Variant> args;
	LocalVector<const Variant *> argptrs;
	args.resize(argc);
	argptrs.resize(argc);
	for (int i = 0; i < argc; i++) {
		args[i] = generic_arg_to_variant(p_gen, i);
		argptrs[i] = &args[i];
	}

	Variant ret;
	GDExtensionCallError err;
	if (bind->spec->is_static) {
		Variant::callp_static(bind->cls_type, bind->method, argptrs.ptr(), argc, ret, err);
	} else {
		Variant self = builtin_to_variant(bind->cls_type, p_gen->GetObject());
		self.callp(bind->method, argptrs.ptr(), argc, ret, err);
		// Write back so mutating methods work on copy-on-write types.
		uint8_t buf[sizeof(Variant) * 4];
		builtin_construct_from_variant(self, bind->cls_type, buf);
		builtin_assign(bind->cls_type, p_gen->GetObject(), buf);
		builtin_destruct(bind->cls_type, buf);
	}
	if (err.error != GDEXTENSION_CALL_OK) {
		asIScriptContext *ctx = asGetActiveContext();
		if (ctx != nullptr) {
			ctx->SetException(String("Invalid call to " + String(bind->method)).utf8().get_data());
		}
		return;
	}
	generic_set_return(p_gen, ret);
}

static void builtin_member_thunk(asIScriptGeneric *p_gen) {
	MemberBindData *bind = static_cast<MemberBindData *>(p_gen->GetAuxiliary());
	bool valid = false;
	if (bind->setter) {
		Variant self = builtin_to_variant(bind->cls_type, p_gen->GetObject());
		self.set_named(bind->member, generic_arg_to_variant(p_gen, 0), valid);
		uint8_t buf[sizeof(Variant) * 4];
		builtin_construct_from_variant(self, bind->cls_type, buf);
		builtin_assign(bind->cls_type, p_gen->GetObject(), buf);
		builtin_destruct(bind->cls_type, buf);
	} else {
		Variant self = builtin_to_variant(bind->cls_type, p_gen->GetObject());
		generic_set_return(p_gen, self.get_named(bind->member, valid));
	}
}

static void builtin_operator_thunk(asIScriptGeneric *p_gen) {
	OperatorBindData *bind = static_cast<OperatorBindData *>(p_gen->GetAuxiliary());
	int type_id = p_gen->GetObjectTypeId();
	Variant self = as_to_variant(p_gen->GetEngine(), p_gen->GetObject(), type_id);
	bool has_rhs = p_gen->GetArgCount() > 0;
	Variant rhs = has_rhs ? generic_arg_to_variant(p_gen, 0) : Variant();

	Variant a = bind->reversed ? rhs : self;
	Variant b = bind->reversed ? self : rhs;

	bool valid = false;
	Variant ret;
	if (bind->synth_cmp) {
		Variant less, greater;
		Variant::evaluate(Variant::OP_LESS, a, b, less, valid);
		Variant::evaluate(Variant::OP_LESS, b, a, greater, valid);
		int cmp = less.operator bool() ? -1 : (greater.operator bool() ? 1 : 0);
		p_gen->SetReturnDWord(asDWORD(cmp));
		return;
	}
	Variant::evaluate(bind->op, a, b, ret, valid);
	if (!valid) {
		asIScriptContext *ctx = asGetActiveContext();
		if (ctx != nullptr) {
			ctx->SetException("Invalid operands for operator");
		}
		return;
	}
	generic_set_return(p_gen, ret);
}

static void builtin_ctor_thunk(asIScriptGeneric *p_gen) {
	CtorBindData *bind = static_cast<CtorBindData *>(p_gen->GetAuxiliary());
	int argc = int(p_gen->GetArgCount());

	LocalVector<Variant> args;
	LocalVector<GDExtensionConstVariantPtr> argptrs;
	args.resize(argc);
	argptrs.resize(argc);
	for (int i = 0; i < argc; i++) {
		args[i] = generic_arg_to_variant(p_gen, i);
		argptrs[i] = args[i]._native_ptr();
	}

	Variant constructed;
	GDExtensionCallError err;
	internal::gdextension_interface_variant_construct(
			GDExtensionVariantType(bind->cls_type), constructed._native_ptr(),
			argptrs.ptr(), argc, &err);
	builtin_construct_from_variant(constructed, bind->cls_type, p_gen->GetObject());
}

static void utility_thunk(asIScriptGeneric *p_gen) {
	UtilityBindData *bind = static_cast<UtilityBindData *>(p_gen->GetAuxiliary());
	int argc = int(p_gen->GetArgCount());

	// Utility functions use ptr-call conventions; for vararg functions every
	// argument is a Variant*.
	LocalVector<Variant> variant_args;
	LocalVector<GDExtensionConstTypePtr> argptrs;
	argptrs.resize(argc);
	if (bind->spec->is_vararg) {
		variant_args.resize(argc);
		for (int i = 0; i < argc; i++) {
			variant_args[i] = generic_arg_to_variant(p_gen, i);
			argptrs[i] = variant_args[i]._native_ptr();
		}
	} else {
		for (int i = 0; i < argc; i++) {
			int type_id = p_gen->GetArgTypeId(asUINT(i));
			if (type_id & asTYPEID_MASK_OBJECT) {
				argptrs[i] = p_gen->GetArgObject(asUINT(i));
			} else {
				argptrs[i] = p_gen->GetAddressOfArg(asUINT(i));
			}
		}
	}

	switch (bind->ret_kind) {
		case 0: {
			bind->fn(nullptr, argptrs.ptr(), argc);
		} break;
		case 1: {
			int64_t ret = 0;
			bind->fn(&ret, argptrs.ptr(), argc);
			p_gen->SetReturnQWord(asQWORD(ret));
		} break;
		case 2: {
			double ret = 0;
			bind->fn(&ret, argptrs.ptr(), argc);
			p_gen->SetReturnDouble(ret);
		} break;
		case 3: {
			GDExtensionBool ret = 0;
			bind->fn(&ret, argptrs.ptr(), argc);
			p_gen->SetReturnByte(ret ? 1 : 0);
		} break;
		case 4: {
			void *ret_mem = p_gen->GetAddressOfReturnLocation();
			builtin_construct(bind->ret_value_type, ret_mem);
			bind->fn(ret_mem, argptrs.ptr(), argc);
		} break;
		case 5: {
			memnew_placement(p_gen->GetAddressOfReturnLocation(), Variant);
			bind->fn(p_gen->GetAddressOfReturnLocation(), argptrs.ptr(), argc);
		} break;
	}
}

// ---------------------------------------------------------------------------
// Container index accessors (hand-written typed thunks)
// ---------------------------------------------------------------------------

static void array_index_get(asIScriptGeneric *p_gen) {
	Array *arr = static_cast<Array *>(p_gen->GetObject());
	int64_t idx = int64_t(p_gen->GetArgQWord(0));
	memnew_placement(p_gen->GetAddressOfReturnLocation(), Variant((*arr)[idx]));
}

static void array_index_set(asIScriptGeneric *p_gen) {
	Array *arr = static_cast<Array *>(p_gen->GetObject());
	int64_t idx = int64_t(p_gen->GetArgQWord(0));
	(*arr)[idx] = generic_arg_to_variant(p_gen, 1);
}

static void dictionary_index_get(asIScriptGeneric *p_gen) {
	Dictionary *dict = static_cast<Dictionary *>(p_gen->GetObject());
	Variant key = generic_arg_to_variant(p_gen, 0);
	memnew_placement(p_gen->GetAddressOfReturnLocation(), Variant((*dict)[key]));
}

static void dictionary_index_set(asIScriptGeneric *p_gen) {
	Dictionary *dict = static_cast<Dictionary *>(p_gen->GetObject());
	Variant key = generic_arg_to_variant(p_gen, 0);
	(*dict)[key] = generic_arg_to_variant(p_gen, 1);
}

template <typename T, typename Elem>
static void packed_index_get(asIScriptGeneric *p_gen) {
	T *arr = static_cast<T *>(p_gen->GetObject());
	int64_t idx = int64_t(p_gen->GetArgQWord(0));
	Variant elem((*arr)[idx]);
	generic_set_return(p_gen, elem);
}

template <typename T, typename Elem>
static void packed_index_set(asIScriptGeneric *p_gen) {
	T *arr = static_cast<T *>(p_gen->GetObject());
	int64_t idx = int64_t(p_gen->GetArgQWord(0));
	Variant value = generic_arg_to_variant(p_gen, 1);
	arr->set(idx, value.operator Elem());
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

struct ValueTypeDef {
	Variant::Type type;
	const char *name;
};

static const ValueTypeDef VALUE_TYPES[] = {
	{ Variant::STRING, "String" },
	{ Variant::VECTOR2, "Vector2" },
	{ Variant::VECTOR2I, "Vector2i" },
	{ Variant::RECT2, "Rect2" },
	{ Variant::RECT2I, "Rect2i" },
	{ Variant::VECTOR3, "Vector3" },
	{ Variant::VECTOR3I, "Vector3i" },
	{ Variant::TRANSFORM2D, "Transform2D" },
	{ Variant::VECTOR4, "Vector4" },
	{ Variant::VECTOR4I, "Vector4i" },
	{ Variant::PLANE, "Plane" },
	{ Variant::QUATERNION, "Quaternion" },
	{ Variant::AABB, "AABB" },
	{ Variant::BASIS, "Basis" },
	{ Variant::TRANSFORM3D, "Transform3D" },
	{ Variant::PROJECTION, "Projection" },
	{ Variant::COLOR, "Color" },
	{ Variant::STRING_NAME, "StringName" },
	{ Variant::NODE_PATH, "NodePath" },
	{ Variant::RID, "RID" },
	{ Variant::CALLABLE, "Callable" },
	{ Variant::SIGNAL, "Signal" },
	{ Variant::DICTIONARY, "Dictionary" },
	{ Variant::ARRAY, "Array" },
	{ Variant::PACKED_BYTE_ARRAY, "PackedByteArray" },
	{ Variant::PACKED_INT32_ARRAY, "PackedInt32Array" },
	{ Variant::PACKED_INT64_ARRAY, "PackedInt64Array" },
	{ Variant::PACKED_FLOAT32_ARRAY, "PackedFloat32Array" },
	{ Variant::PACKED_FLOAT64_ARRAY, "PackedFloat64Array" },
	{ Variant::PACKED_STRING_ARRAY, "PackedStringArray" },
	{ Variant::PACKED_VECTOR2_ARRAY, "PackedVector2Array" },
	{ Variant::PACKED_VECTOR3_ARRAY, "PackedVector3Array" },
	{ Variant::PACKED_COLOR_ARRAY, "PackedColorArray" },
	{ Variant::PACKED_VECTOR4_ARRAY, "PackedVector4Array" },
};

void register_builtin_types(asIScriptEngine *p_engine) {
	// Value type shells + lifecycle.
	for (const ValueTypeDef &def : VALUE_TYPES) {
		void *aux = reinterpret_cast<void *>(uintptr_t(def.type));
		AS_CHECK(p_engine->RegisterObjectType(def.name, int(builtin_size(def.type)),
				asOBJ_VALUE | asDWORD(builtin_type_traits(def.type))));
		AS_CHECK(p_engine->RegisterObjectBehaviour(def.name, asBEHAVE_CONSTRUCT, "void f()",
				asFUNCTION(value_construct), asCALL_GENERIC, aux));
		AS_CHECK(p_engine->RegisterObjectBehaviour(def.name, asBEHAVE_CONSTRUCT,
				(String("void f(const ") + def.name + " &in)").utf8().get_data(),
				asFUNCTION(value_copy_construct), asCALL_GENERIC, aux));
		AS_CHECK(p_engine->RegisterObjectBehaviour(def.name, asBEHAVE_DESTRUCT, "void f()",
				asFUNCTION(value_destruct), asCALL_GENERIC, aux));
		AS_CHECK(p_engine->RegisterObjectMethod(def.name,
				(String(def.name) + " &opAssign(const " + def.name + " &in)").utf8().get_data(),
				asFUNCTION(value_assign), asCALL_GENERIC, aux));
		bridge_register_value_type(def.type, p_engine->GetTypeIdByDecl(def.name));
	}

	string_factory = memnew(GodotStringFactory);
	AS_CHECK(p_engine->RegisterStringFactory("String", string_factory));

	// Variant.
	AS_CHECK(p_engine->RegisterObjectType("Variant", int(sizeof(Variant)),
			asOBJ_VALUE | asGetTypeTraits<Variant>()));
	AS_CHECK(p_engine->RegisterObjectBehaviour("Variant", asBEHAVE_CONSTRUCT, "void f()",
			asFUNCTION(variant_construct_default), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterObjectBehaviour("Variant", asBEHAVE_DESTRUCT, "void f()",
			asFUNCTION(variant_destruct), asCALL_GENERIC));
	bridge_register_value_type(Variant::VARIANT_MAX, p_engine->GetTypeIdByDecl("Variant"));

	// Global enums (Key, Error, Side, ...).
	for (int i = 0; i < gen::global_enums_count; i++) {
		const gen::EnumSpec &en = gen::global_enums[i];
		AS_CHECK(p_engine->RegisterEnum(en.name));
		for (int v = 0; v < en.count; v++) {
			const gen::EnumValSpec &val = gen::global_enum_values[en.first_value + v];
			if (val.value >= INT32_MIN && val.value <= INT32_MAX) {
				AS_CHECK(p_engine->RegisterEnumValue(en.name, val.name, int(val.value)));
			} else {
				int64_t *storage = static_cast<int64_t *>(memalloc(sizeof(int64_t)));
				*storage = val.value;
				big_constants.push_back(storage);
				AS_CHECK(p_engine->RegisterGlobalProperty(
						(String("const int64 ") + val.name).utf8().get_data(), storage));
			}
		}
	}
}

// Builds the parameter list for a generated method/utility declaration.
// Returns false if a parameter type cannot be represented.
static bool build_params(const gen::ArgSpec *p_args, int p_argc, int p_extra_variants, String &r_params) {
	PackedStringArray parts;
	bool defaults_ok = true;
	for (int i = 0; i < p_argc; i++) {
		const gen::ArgSpec &arg = p_args[i];
		String as_type = as_type_from_godot_name(String::utf8(arg.type));
		String part = as_param_decl(as_type) + " " + sanitize_identifier(String::utf8(arg.name));
		if (arg.default_value != nullptr && defaults_ok) {
			String def = String::utf8(arg.default_value);
			String encoded;
			if (def == "null" || def == "true" || def == "false" || def.is_valid_int() || def.is_valid_float()) {
				encoded = def;
			} else if (def.begins_with("\"") || def.begins_with("&\"") || def.begins_with("^\"")) {
				String raw = def.trim_prefix("&").trim_prefix("^");
				if (as_type == "StringName") {
					encoded = "StringName(" + raw + ")";
				} else if (as_type == "NodePath") {
					encoded = "NodePath(" + raw + ")";
				} else {
					encoded = raw;
				}
			} else if (def == "[]") {
				encoded = "Array()";
			} else if (def == "{}") {
				encoded = "Dictionary()";
			} else if (def.contains("(") && builtin_type_from_name(def.get_slice("(", 0)) != Variant::VARIANT_MAX) {
				encoded = def; // e.g. "Vector2(0, 0)"
			}
			if (encoded.is_empty()) {
				defaults_ok = false; // must drop all remaining defaults
			} else {
				part += " = " + encoded;
			}
		} else if (arg.default_value != nullptr) {
			// A previous default could not be encoded; later defaults dropped too.
		}
		parts.push_back(part);
	}
	for (int i = 0; i < p_extra_variants; i++) {
		parts.push_back(vformat("const Variant &in vararg%d", i));
	}
	r_params = String(", ").join(parts);
	return true;
}

void register_builtin_members(asIScriptEngine *p_engine) {
	// Variant conversions: constructors from every type, opConv to every type.
	{
		PackedStringArray conv_types;
		conv_types.push_back("bool");
		conv_types.push_back("int64");
		conv_types.push_back("double");
		conv_types.push_back("int");
		conv_types.push_back("float");
		for (const ValueTypeDef &def : VALUE_TYPES) {
			conv_types.push_back(def.name);
		}
		conv_types.push_back("godot::Object@");
		for (String type : conv_types) {
			String param = type.ends_with("@") ? type : (type == "bool" || type == "int64" || type == "double" || type == "int" || type == "float") ? type : "const " + type + " &in";
			AS_CHECK(p_engine->RegisterObjectBehaviour("Variant", asBEHAVE_CONSTRUCT,
					(String("void f(") + param + ")").utf8().get_data(),
					asFUNCTION(variant_construct_copyish), asCALL_GENERIC));
			if (type != "int" && type != "float") { // avoid ambiguous conversions
				AS_CHECK(p_engine->RegisterObjectMethod("Variant",
						(type + " opConv() const").utf8().get_data(),
						asFUNCTION(variant_opconv), asCALL_GENERIC));
			}
		}
		AS_CHECK(p_engine->RegisterObjectBehaviour("Variant", asBEHAVE_CONSTRUCT,
				"void f(const Variant &in)", asFUNCTION(variant_construct_copyish), asCALL_GENERIC));
		AS_CHECK(p_engine->RegisterObjectMethod("Variant", "Variant &opAssign(const Variant &in)",
				asFUNCTION(variant_assign_any), asCALL_GENERIC));
		AS_CHECK(p_engine->RegisterObjectMethod("Variant", "bool opEquals(const Variant &in) const",
				asFUNCTION(variant_opequals), asCALL_GENERIC));
		AS_CHECK(p_engine->RegisterObjectMethod("Variant", "int get_type() const",
				asFUNCTION(variant_get_type), asCALL_GENERIC));
		AS_CHECK(p_engine->RegisterObjectMethod("Variant", "String to_string() const",
				asFUNCTION(variant_to_string), asCALL_GENERIC));
	}

	// Constructors from the generated table.
	for (int i = 0; i < gen::builtin_ctors_count; i++) {
		const gen::CtorSpec &ct = gen::builtin_ctors[i];
		if (ct.argc == 0) {
			continue; // default ctor already registered
		}
		if (ct.argc == 1 && String::utf8(ct.args[0].type) == String::utf8(ct.cls)) {
			continue; // copy ctor already registered
		}
		String params;
		build_params(ct.args, ct.argc, 0, params);
		Variant::Type cls_type = builtin_type_from_name(String::utf8(ct.cls));
		CtorBindData *bind = memnew(CtorBindData);
		bind->cls_type = cls_type;
		bind->spec = &ct;
		ctor_binds.push_back(bind);
		AS_CHECK(p_engine->RegisterObjectBehaviour(ct.cls, asBEHAVE_CONSTRUCT,
				(String("void f(") + params + ")").utf8().get_data(),
				asFUNCTION(builtin_ctor_thunk), asCALL_GENERIC, bind));
	}

	// Method/member cross-referencing for property-accessor decisions.
	HashMap<String, HashSet<String>> method_names_by_class;
	for (int i = 0; i < gen::builtin_methods_count; i++) {
		method_names_by_class[String::utf8(gen::builtin_methods[i].cls)].insert(String::utf8(gen::builtin_methods[i].as_name));
	}
	HashMap<String, HashSet<String>> member_names_by_class;
	for (int i = 0; i < gen::builtin_members_count; i++) {
		member_names_by_class[String::utf8(gen::builtin_members[i].cls)].insert(String::utf8(gen::builtin_members[i].name));
	}

	// Methods from the generated table.
	for (int i = 0; i < gen::builtin_methods_count; i++) {
		const gen::MethodSpec &m = gen::builtin_methods[i];
		Variant::Type cls_type = builtin_type_from_name(String::utf8(m.cls));
		String ret_type = m.return_type != nullptr ? as_type_from_godot_name(String::utf8(m.return_type)) : String("void");

		MethodBindData *bind = memnew(MethodBindData);
		bind->method = StringName(m.godot_name);
		bind->cls_type = cls_type;
		bind->spec = &m;
		method_binds.push_back(bind);

		int vararg_lo = 0, vararg_hi = 0;
		if (m.is_vararg) {
			vararg_hi = 8;
		}
		// get_x/set_x methods matching a member become property accessors so
		// `transform.origin` resolves.
		String method_name = String::utf8(m.as_name);
		String property_suffix;
		if (!m.is_static && !m.is_vararg) {
			HashSet<String> *members = member_names_by_class.getptr(String::utf8(m.cls));
			if (members != nullptr) {
				bool returns_value = m.return_type != nullptr;
				if (m.argc == 0 && returns_value && method_name.begins_with("get_") && members->has(method_name.trim_prefix("get_"))) {
					property_suffix = " property";
				} else if (m.argc == 1 && !returns_value && method_name.begins_with("set_") && members->has(method_name.trim_prefix("set_"))) {
					property_suffix = " property";
				}
			}
		}

		for (int extra = vararg_lo; extra <= vararg_hi; extra++) {
			String params;
			build_params(m.args, m.argc, extra, params);
			String decl = ret_type + " " + m.as_name + "(" + params + ")";
			if (!m.is_static) {
				decl += " const";
			}
			decl += property_suffix;
			if (m.is_static) {
				// Static builtin methods live in a namespace named after the type.
				p_engine->SetDefaultNamespace(m.cls);
				int r = p_engine->RegisterGlobalFunction(decl.utf8().get_data(),
						asFUNCTION(builtin_method_thunk), asCALL_GENERIC, bind);
				p_engine->SetDefaultNamespace("");
				if (r < 0) {
					// Fall back to a prefixed global function name.
					String fallback = ret_type + " " + m.cls + "_" + m.as_name + "(" + params + ")";
					AS_CHECK(p_engine->RegisterGlobalFunction(fallback.utf8().get_data(),
							asFUNCTION(builtin_method_thunk), asCALL_GENERIC, bind));
				}
			} else {
				AS_CHECK(p_engine->RegisterObjectMethod(m.cls, decl.utf8().get_data(),
						asFUNCTION(builtin_method_thunk), asCALL_GENERIC, bind));
			}
		}
	}

	// Members (Vector2.x, Color.r, ...) as property accessors. Skipped when a
	// real get_/set_ method already exists (registered above as accessor).
	for (int i = 0; i < gen::builtin_members_count; i++) {
		const gen::MemberSpec &mem = gen::builtin_members[i];
		Variant::Type cls_type = builtin_type_from_name(String::utf8(mem.cls));
		String as_type = as_type_from_godot_name(String::utf8(mem.type));
		HashSet<String> *methods = method_names_by_class.getptr(String::utf8(mem.cls));
		bool has_getter_method = methods != nullptr && methods->has(String("get_") + mem.name);
		bool has_setter_method = methods != nullptr && methods->has(String("set_") + mem.name);
		if (has_getter_method && has_setter_method) {
			continue;
		}
		if (has_getter_method || has_setter_method) {
			// Register only the missing half.
		}

		if (!has_getter_method) {
			MemberBindData *getter = memnew(MemberBindData);
			getter->member = StringName(mem.name);
			getter->cls_type = cls_type;
			getter->setter = false;
			member_binds.push_back(getter);
			AS_CHECK(p_engine->RegisterObjectMethod(mem.cls,
					(as_type + " get_" + mem.name + "() const property").utf8().get_data(),
					asFUNCTION(builtin_member_thunk), asCALL_GENERIC, getter));
		}
		if (!has_setter_method) {
			MemberBindData *setter = memnew(MemberBindData);
			setter->member = StringName(mem.name);
			setter->cls_type = cls_type;
			setter->setter = true;
			member_binds.push_back(setter);
			AS_CHECK(p_engine->RegisterObjectMethod(mem.cls,
					(String("void set_") + mem.name + "(" + as_param_decl(as_type) + " value) property").utf8().get_data(),
					asFUNCTION(builtin_member_thunk), asCALL_GENERIC, setter));
		}
	}

	// Operators.
	struct OpMap {
		const char *token;
		Variant::Operator op;
		const char *as_name;
		const char *as_name_reversed;
	};
	static const OpMap OP_MAP[] = {
		{ "+", Variant::OP_ADD, "opAdd", "opAdd_r" },
		{ "-", Variant::OP_SUBTRACT, "opSub", "opSub_r" },
		{ "*", Variant::OP_MULTIPLY, "opMul", "opMul_r" },
		{ "/", Variant::OP_DIVIDE, "opDiv", "opDiv_r" },
		{ "%", Variant::OP_MODULE, "opMod", "opMod_r" },
		{ "**", Variant::OP_POWER, "opPow", "opPow_r" },
		{ "==", Variant::OP_EQUAL, "opEquals", "opEquals" },
		{ "unary-", Variant::OP_NEGATE, "opNeg", nullptr },
		{ "~", Variant::OP_BIT_NEGATE, "opCom", nullptr },
		{ "<<", Variant::OP_SHIFT_LEFT, "opShl", "opShl_r" },
		{ ">>", Variant::OP_SHIFT_RIGHT, "opShr", "opShr_r" },
		{ "&", Variant::OP_BIT_AND, "opAnd", "opAnd_r" },
		{ "|", Variant::OP_BIT_OR, "opOr", "opOr_r" },
		{ "^", Variant::OP_BIT_XOR, "opXor", "opXor_r" },
		{ "<", Variant::OP_LESS, "opCmp", "opCmp" },
	};
	for (int i = 0; i < gen::builtin_operators_count; i++) {
		const gen::OperatorSpec &op = gen::builtin_operators[i];
		String token = String::utf8(op.op);
		const OpMap *mapping = nullptr;
		for (const OpMap &candidate : OP_MAP) {
			if (token == candidate.token) {
				mapping = &candidate;
				break;
			}
		}
		if (mapping == nullptr) {
			continue; // not/in/and/or/xor/!=/<=/>/>= are derived or unsupported
		}
		const char *as_name = op.reversed ? mapping->as_name_reversed : mapping->as_name;
		if (as_name == nullptr) {
			continue;
		}
		bool synth_cmp = mapping->op == Variant::OP_LESS;
		// Comparisons against Variant right-hand sides create ambiguity; skip.
		if (synth_cmp && (op.right_type == nullptr || String::utf8(op.right_type) == "Variant")) {
			continue;
		}

		String ret_type = synth_cmp ? String("int") : as_type_from_godot_name(String::utf8(op.return_type));
		String params;
		if (op.right_type != nullptr) {
			params = as_param_decl(as_type_from_godot_name(String::utf8(op.right_type)));
		}

		OperatorBindData *bind = memnew(OperatorBindData);
		bind->op = mapping->op;
		bind->reversed = op.reversed;
		bind->synth_cmp = synth_cmp;
		operator_binds.push_back(bind);

		String decl = ret_type + " " + as_name + "(" + params + ") const";
		AS_CHECK(p_engine->RegisterObjectMethod(op.cls, decl.utf8().get_data(),
				asFUNCTION(builtin_operator_thunk), asCALL_GENERIC, bind));
	}

	// Index accessors.
	AS_CHECK(p_engine->RegisterObjectMethod("Array", "Variant get_opIndex(int64) const property",
			asFUNCTION(array_index_get), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterObjectMethod("Array", "void set_opIndex(int64, const Variant &in) property",
			asFUNCTION(array_index_set), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterObjectMethod("Dictionary", "Variant get_opIndex(const Variant &in) const property",
			asFUNCTION(dictionary_index_get), asCALL_GENERIC));
	AS_CHECK(p_engine->RegisterObjectMethod("Dictionary", "void set_opIndex(const Variant &in, const Variant &in) property",
			asFUNCTION(dictionary_index_set), asCALL_GENERIC));

#define REGISTER_PACKED_INDEX(CLS, CPP, ELEM_AS, ELEM_CPP)                                            \
	AS_CHECK(p_engine->RegisterObjectMethod(CLS, ELEM_AS " get_opIndex(int64) const property",       \
			asFUNCTION((packed_index_get<CPP, ELEM_CPP>)), asCALL_GENERIC));                          \
	AS_CHECK(p_engine->RegisterObjectMethod(CLS, "void set_opIndex(int64, " ELEM_AS ") property",    \
			asFUNCTION((packed_index_set<CPP, ELEM_CPP>)), asCALL_GENERIC));

	REGISTER_PACKED_INDEX("PackedByteArray", PackedByteArray, "int64", int64_t)
	REGISTER_PACKED_INDEX("PackedInt32Array", PackedInt32Array, "int64", int64_t)
	REGISTER_PACKED_INDEX("PackedInt64Array", PackedInt64Array, "int64", int64_t)
	REGISTER_PACKED_INDEX("PackedFloat32Array", PackedFloat32Array, "double", double)
	REGISTER_PACKED_INDEX("PackedFloat64Array", PackedFloat64Array, "double", double)
#undef REGISTER_PACKED_INDEX
#define REGISTER_PACKED_INDEX_OBJ(CLS, CPP, ELEM_AS, ELEM_CPP)                                                       \
	AS_CHECK(p_engine->RegisterObjectMethod(CLS, ELEM_AS " get_opIndex(int64) const property",                       \
			asFUNCTION((packed_index_get<CPP, ELEM_CPP>)), asCALL_GENERIC));                                         \
	AS_CHECK(p_engine->RegisterObjectMethod(CLS, "void set_opIndex(int64, const " ELEM_AS " &in) property",          \
			asFUNCTION((packed_index_set<CPP, ELEM_CPP>)), asCALL_GENERIC));

	REGISTER_PACKED_INDEX_OBJ("PackedStringArray", PackedStringArray, "String", String)
	REGISTER_PACKED_INDEX_OBJ("PackedVector2Array", PackedVector2Array, "Vector2", Vector2)
	REGISTER_PACKED_INDEX_OBJ("PackedVector3Array", PackedVector3Array, "Vector3", Vector3)
	REGISTER_PACKED_INDEX_OBJ("PackedColorArray", PackedColorArray, "Color", Color)
	REGISTER_PACKED_INDEX_OBJ("PackedVector4Array", PackedVector4Array, "Vector4", Vector4)
#undef REGISTER_PACKED_INDEX_OBJ

	// Utility functions (print, lerp, clamp, randi, ...).
	for (int i = 0; i < gen::utility_functions_count; i++) {
		const gen::UtilitySpec &u = gen::utility_functions[i];
		String ret_name = u.return_type != nullptr ? String::utf8(u.return_type) : String();

		UtilityBindData *bind = memnew(UtilityBindData);
		bind->name = StringName(u.name);
		bind->spec = &u;
		StringName fn_name(u.name);
		bind->fn = internal::gdextension_interface_variant_get_ptr_utility_function(
				fn_name._native_ptr(), u.hash);
		if (bind->fn == nullptr) {
			memdelete(bind);
			continue;
		}
		utility_binds.push_back(bind);

		String ret_type = "void";
		if (!ret_name.is_empty()) {
			if (ret_name == "int" || ret_name.begins_with("enum::") || ret_name.begins_with("bitfield::")) {
				bind->ret_kind = 1;
				ret_type = "int64";
			} else if (ret_name == "float") {
				bind->ret_kind = 2;
				ret_type = "double";
			} else if (ret_name == "bool") {
				bind->ret_kind = 3;
				ret_type = "bool";
			} else if (ret_name == "Variant") {
				bind->ret_kind = 5;
				ret_type = "Variant";
			} else if (builtin_type_from_name(ret_name) != Variant::VARIANT_MAX) {
				bind->ret_kind = 4;
				bind->ret_value_type = builtin_type_from_name(ret_name);
				ret_type = ret_name;
			} else {
				continue; // Object-returning utilities handled elsewhere
			}
		}

		int vararg_lo = 0, vararg_hi = 0;
		int fixed_argc = u.argc;
		if (u.is_vararg) {
			// Vararg utilities take Variant-only args; the json argc is the
			// suggested minimum.
			vararg_lo = 0;
			vararg_hi = 8;
			fixed_argc = 0;
		}
		for (int extra = vararg_lo; extra <= vararg_hi; extra++) {
			String params;
			build_params(u.args, fixed_argc, extra, params);
			String decl = ret_type + " " + u.name + "(" + params + ")";
			AS_CHECK(p_engine->RegisterGlobalFunction(decl.utf8().get_data(),
					asFUNCTION(utility_thunk), asCALL_GENERIC, bind));
		}
	}
}

} // namespace gdas
