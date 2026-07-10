#include "variant_bridge.h"

#include "native_classes.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

// X(enum_name, cpp_type, as_name) for every builtin value type we register.
// bool/int/float are AngelScript primitives and are not in this table.
#define BUILTIN_VALUE_TYPES(X)                                    \
	X(STRING, String, "String")                                   \
	X(VECTOR2, Vector2, "Vector2")                                \
	X(VECTOR2I, Vector2i, "Vector2i")                             \
	X(RECT2, Rect2, "Rect2")                                      \
	X(RECT2I, Rect2i, "Rect2i")                                   \
	X(VECTOR3, Vector3, "Vector3")                                \
	X(VECTOR3I, Vector3i, "Vector3i")                             \
	X(TRANSFORM2D, Transform2D, "Transform2D")                    \
	X(VECTOR4, Vector4, "Vector4")                                \
	X(VECTOR4I, Vector4i, "Vector4i")                             \
	X(PLANE, Plane, "Plane")                                      \
	X(QUATERNION, Quaternion, "Quaternion")                       \
	X(AABB, AABB, "AABB")                                         \
	X(BASIS, Basis, "Basis")                                      \
	X(TRANSFORM3D, Transform3D, "Transform3D")                    \
	X(PROJECTION, Projection, "Projection")                       \
	X(COLOR, Color, "Color")                                      \
	X(STRING_NAME, StringName, "StringName")                      \
	X(NODE_PATH, NodePath, "NodePath")                            \
	X(RID, RID, "RID")                                            \
	X(CALLABLE, Callable, "Callable")                             \
	X(SIGNAL, Signal, "Signal")                                   \
	X(DICTIONARY, Dictionary, "Dictionary")                       \
	X(ARRAY, Array, "Array")                                      \
	X(PACKED_BYTE_ARRAY, PackedByteArray, "PackedByteArray")      \
	X(PACKED_INT32_ARRAY, PackedInt32Array, "PackedInt32Array")   \
	X(PACKED_INT64_ARRAY, PackedInt64Array, "PackedInt64Array")   \
	X(PACKED_FLOAT32_ARRAY, PackedFloat32Array, "PackedFloat32Array") \
	X(PACKED_FLOAT64_ARRAY, PackedFloat64Array, "PackedFloat64Array") \
	X(PACKED_STRING_ARRAY, PackedStringArray, "PackedStringArray") \
	X(PACKED_VECTOR2_ARRAY, PackedVector2Array, "PackedVector2Array") \
	X(PACKED_VECTOR3_ARRAY, PackedVector3Array, "PackedVector3Array") \
	X(PACKED_COLOR_ARRAY, PackedColorArray, "PackedColorArray")   \
	X(PACKED_VECTOR4_ARRAY, PackedVector4Array, "PackedVector4Array")

Variant::Type builtin_type_from_name(const String &p_name) {
	static HashMap<String, Variant::Type> map = []() {
		HashMap<String, Variant::Type> m;
		m.insert("bool", Variant::BOOL);
		m.insert("int", Variant::INT);
		m.insert("float", Variant::FLOAT);
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) m.insert(AS_NAME, Variant::ENUM_NAME);
		BUILTIN_VALUE_TYPES(X)
#undef X
		return m;
	}();
	const Variant::Type *found = map.getptr(p_name);
	return found ? *found : Variant::VARIANT_MAX;
}

size_t builtin_size(Variant::Type p_type) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) \
	case Variant::ENUM_NAME:            \
		return sizeof(::godot::CPP_TYPE);
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			return 0;
	}
}

int builtin_type_traits(Variant::Type p_type) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) \
	case Variant::ENUM_NAME:            \
		return asGetTypeTraits<::godot::CPP_TYPE>();
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			return 0;
	}
}

void builtin_construct(Variant::Type p_type, void *p_mem) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) \
	case Variant::ENUM_NAME:            \
		memnew_placement(p_mem, ::godot::CPP_TYPE); \
		break;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			break;
	}
}

void builtin_copy_construct(Variant::Type p_type, void *p_mem, const void *p_src) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME)                                     \
	case Variant::ENUM_NAME:                                                \
		memnew_placement(p_mem, ::godot::CPP_TYPE(*static_cast<const ::godot::CPP_TYPE *>(p_src))); \
		break;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			break;
	}
}

void builtin_destruct(Variant::Type p_type, void *p_mem) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME)          \
	case Variant::ENUM_NAME:                     \
		static_cast<::godot::CPP_TYPE *>(p_mem)->~CPP_TYPE(); \
		break;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			break;
	}
}

void builtin_assign(Variant::Type p_type, void *p_dst, const void *p_src) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME)                                              \
	case Variant::ENUM_NAME:                                                         \
		*static_cast<::godot::CPP_TYPE *>(p_dst) = *static_cast<const ::godot::CPP_TYPE *>(p_src);     \
		break;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			break;
	}
}

Variant builtin_to_variant(Variant::Type p_type, const void *p_src) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) \
	case Variant::ENUM_NAME:            \
		return Variant(*static_cast<const ::godot::CPP_TYPE *>(p_src));
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			return Variant();
	}
}

void builtin_construct_from_variant(const Variant &p_value, Variant::Type p_type, void *p_dst) {
	switch (p_type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME)                    \
	case Variant::ENUM_NAME:                               \
		memnew_placement(p_dst, ::godot::CPP_TYPE(p_value.operator ::godot::CPP_TYPE())); \
		break;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Type id registry
// ---------------------------------------------------------------------------

static int variant_type_to_as_id[Variant::VARIANT_MAX + 1] = {};
static HashMap<int, Variant::Type> as_id_to_variant_type;
static int variant_as_type_id = 0; // the registered `Variant` value type

void bridge_register_value_type(Variant::Type p_type, int p_as_type_id) {
	if (p_type == Variant::VARIANT_MAX) {
		variant_as_type_id = p_as_type_id;
	} else {
		variant_type_to_as_id[p_type] = p_as_type_id;
	}
	as_id_to_variant_type.insert(p_as_type_id, p_type);
}

int bridge_get_as_type_id(Variant::Type p_type) {
	if (p_type == Variant::VARIANT_MAX) {
		return variant_as_type_id;
	}
	return variant_type_to_as_id[p_type];
}

Variant::Type bridge_get_variant_type(int p_as_type_id) {
	const Variant::Type *found = as_id_to_variant_type.getptr(p_as_type_id);
	return found ? *found : Variant::VARIANT_MAX;
}

static bool is_variant_type_id(int p_type_id) {
	return p_type_id == variant_as_type_id;
}

// ---------------------------------------------------------------------------
// Object helpers
// ---------------------------------------------------------------------------

// Cached index of the `__self` property on script-base classes; stored +1 so
// zero means "not resolved yet". Attached to asITypeInfo user data.
constexpr asPWORD SELF_PROP_USERDATA = 0x600D0002;

static int find_self_property(asITypeInfo *p_type) {
	asPWORD cached = reinterpret_cast<asPWORD>(p_type->GetUserData(SELF_PROP_USERDATA));
	if (cached != 0) {
		return int(cached) - 2; // -1 sentinel for "has none"
	}
	int found = -1;
	for (asUINT i = 0; i < p_type->GetPropertyCount(); i++) {
		const char *name = nullptr;
		p_type->GetProperty(i, &name);
		if (name && strcmp(name, "__self") == 0) {
			found = int(i);
			break;
		}
	}
	p_type->SetUserData(reinterpret_cast<void *>(asPWORD(found + 2)), SELF_PROP_USERDATA);
	return found;
}

Object *as_handle_to_object(void *p_obj, asITypeInfo *p_type) {
	if (p_obj == nullptr || p_type == nullptr) {
		return nullptr;
	}
	if (p_type->GetFlags() & asOBJ_SCRIPT_OBJECT) {
		asIScriptObject *script_obj = static_cast<asIScriptObject *>(p_obj);
		asITypeInfo *obj_type = script_obj->GetObjectType();
		int self_prop = find_self_property(obj_type);
		if (self_prop < 0) {
			return nullptr;
		}
		void **self_addr = static_cast<void **>(script_obj->GetAddressOfProperty(asUINT(self_prop)));
		return static_cast<Object *>(*self_addr);
	}
	if (p_type->GetUserData(NATIVE_CLASS_USERDATA) != nullptr) {
		return static_cast<Object *>(p_obj);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

static Variant primitive_to_variant(void *p_ref, int p_type_id) {
	switch (p_type_id) {
		case asTYPEID_BOOL:
			return Variant(*static_cast<bool *>(p_ref));
		case asTYPEID_INT8:
			return Variant(int64_t(*static_cast<int8_t *>(p_ref)));
		case asTYPEID_INT16:
			return Variant(int64_t(*static_cast<int16_t *>(p_ref)));
		case asTYPEID_INT32:
			return Variant(int64_t(*static_cast<int32_t *>(p_ref)));
		case asTYPEID_INT64:
			return Variant(*static_cast<int64_t *>(p_ref));
		case asTYPEID_UINT8:
			return Variant(int64_t(*static_cast<uint8_t *>(p_ref)));
		case asTYPEID_UINT16:
			return Variant(int64_t(*static_cast<uint16_t *>(p_ref)));
		case asTYPEID_UINT32:
			return Variant(int64_t(*static_cast<uint32_t *>(p_ref)));
		case asTYPEID_UINT64:
			return Variant(int64_t(*static_cast<uint64_t *>(p_ref)));
		case asTYPEID_FLOAT:
			return Variant(double(*static_cast<float *>(p_ref)));
		case asTYPEID_DOUBLE:
			return Variant(*static_cast<double *>(p_ref));
		default:
			return Variant();
	}
}

static bool variant_to_primitive(const Variant &p_value, void *p_ref, int p_type_id) {
	switch (p_type_id) {
		case asTYPEID_BOOL:
			*static_cast<bool *>(p_ref) = p_value.operator bool();
			return true;
		case asTYPEID_INT8:
			*static_cast<int8_t *>(p_ref) = int8_t(p_value.operator int64_t());
			return true;
		case asTYPEID_INT16:
			*static_cast<int16_t *>(p_ref) = int16_t(p_value.operator int64_t());
			return true;
		case asTYPEID_INT32:
			*static_cast<int32_t *>(p_ref) = int32_t(p_value.operator int64_t());
			return true;
		case asTYPEID_INT64:
			*static_cast<int64_t *>(p_ref) = p_value.operator int64_t();
			return true;
		case asTYPEID_UINT8:
			*static_cast<uint8_t *>(p_ref) = uint8_t(p_value.operator int64_t());
			return true;
		case asTYPEID_UINT16:
			*static_cast<uint16_t *>(p_ref) = uint16_t(p_value.operator int64_t());
			return true;
		case asTYPEID_UINT32:
			*static_cast<uint32_t *>(p_ref) = uint32_t(p_value.operator int64_t());
			return true;
		case asTYPEID_UINT64:
			*static_cast<uint64_t *>(p_ref) = uint64_t(p_value.operator int64_t());
			return true;
		case asTYPEID_FLOAT:
			*static_cast<float *>(p_ref) = float(p_value.operator double());
			return true;
		case asTYPEID_DOUBLE:
			*static_cast<double *>(p_ref) = p_value.operator double();
			return true;
		default:
			return false;
	}
}

Variant as_to_variant(asIScriptEngine *p_engine, void *p_ref, int p_type_id) {
	if (p_ref == nullptr) {
		return Variant();
	}
	if (p_type_id == asTYPEID_VOID) {
		return Variant();
	}
	if (!(p_type_id & asTYPEID_MASK_OBJECT)) {
		// Primitive or enum.
		if (p_type_id > asTYPEID_DOUBLE) {
			// Application/script enum: 32-bit int.
			return Variant(int64_t(*static_cast<int32_t *>(p_ref)));
		}
		return primitive_to_variant(p_ref, p_type_id);
	}

	void *obj = p_ref;
	if (p_type_id & asTYPEID_OBJHANDLE) {
		obj = *static_cast<void **>(p_ref);
		if (obj == nullptr) {
			return Variant();
		}
	}

	if (is_variant_type_id(p_type_id & ~asTYPEID_OBJHANDLE)) {
		return *static_cast<Variant *>(obj);
	}

	Variant::Type vtype = bridge_get_variant_type(p_type_id & ~asTYPEID_OBJHANDLE);
	if (vtype != Variant::VARIANT_MAX) {
		return builtin_to_variant(vtype, obj);
	}

	asITypeInfo *type = p_engine->GetTypeInfoById(p_type_id);
	Object *godot_obj = as_handle_to_object(obj, type);
	if (godot_obj != nullptr || (type != nullptr && ((type->GetFlags() & asOBJ_SCRIPT_OBJECT) || type->GetUserData(NATIVE_CLASS_USERDATA)))) {
		return Variant(godot_obj);
	}
	return Variant();
}

Variant generic_arg_to_variant(asIScriptGeneric *p_gen, int p_index) {
	int type_id = p_gen->GetArgTypeId(asUINT(p_index));
	if (type_id & asTYPEID_MASK_OBJECT) {
		void *obj = p_gen->GetArgObject(asUINT(p_index));
		if (obj == nullptr) {
			return Variant();
		}
		if (is_variant_type_id(type_id & ~asTYPEID_OBJHANDLE)) {
			return *static_cast<Variant *>(obj);
		}
		Variant::Type vtype = bridge_get_variant_type(type_id & ~asTYPEID_OBJHANDLE);
		if (vtype != Variant::VARIANT_MAX) {
			return builtin_to_variant(vtype, obj);
		}
		asITypeInfo *type = p_gen->GetEngine()->GetTypeInfoById(type_id);
		return Variant(as_handle_to_object(obj, type));
	}
	return as_to_variant(p_gen->GetEngine(), p_gen->GetAddressOfArg(asUINT(p_index)), type_id);
}

void generic_set_return(asIScriptGeneric *p_gen, const Variant &p_value) {
	asIScriptFunction *func = p_gen->GetFunction();
	int type_id = func->GetReturnTypeId();
	if (type_id == asTYPEID_VOID) {
		return;
	}
	if (!(type_id & asTYPEID_MASK_OBJECT)) {
		if (type_id > asTYPEID_DOUBLE) {
			p_gen->SetReturnDWord(asDWORD(int32_t(p_value.operator int64_t())));
			return;
		}
		switch (type_id) {
			case asTYPEID_BOOL:
				p_gen->SetReturnByte(p_value.operator bool() ? 1 : 0);
				return;
			case asTYPEID_INT8:
			case asTYPEID_UINT8:
				p_gen->SetReturnByte(asBYTE(p_value.operator int64_t()));
				return;
			case asTYPEID_INT16:
			case asTYPEID_UINT16:
				p_gen->SetReturnWord(asWORD(p_value.operator int64_t()));
				return;
			case asTYPEID_INT32:
			case asTYPEID_UINT32:
				p_gen->SetReturnDWord(asDWORD(p_value.operator int64_t()));
				return;
			case asTYPEID_INT64:
			case asTYPEID_UINT64:
				p_gen->SetReturnQWord(asQWORD(p_value.operator int64_t()));
				return;
			case asTYPEID_FLOAT:
				p_gen->SetReturnFloat(float(p_value.operator double()));
				return;
			case asTYPEID_DOUBLE:
				p_gen->SetReturnDouble(p_value.operator double());
				return;
			default:
				return;
		}
	}

	int base_id = type_id & ~asTYPEID_OBJHANDLE;
	if (type_id & asTYPEID_OBJHANDLE) {
		asITypeInfo *type = p_gen->GetEngine()->GetTypeInfoById(base_id);
		Object *obj = p_value.operator Object *();
		void *handle = object_to_as_handle(obj, type);
		p_gen->SetReturnObject(handle);
		if (handle != nullptr && type != nullptr && (type->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
			// object_to_as_handle returned a strong ref; SetReturnObject added
			// its own, so drop ours.
			p_gen->GetEngine()->ReleaseScriptObject(handle, type);
		}
		return;
	}
	if (is_variant_type_id(base_id)) {
		memnew_placement(p_gen->GetAddressOfReturnLocation(), Variant(p_value));
		return;
	}
	Variant::Type vtype = bridge_get_variant_type(base_id);
	if (vtype != Variant::VARIANT_MAX) {
		builtin_construct_from_variant(p_value, vtype, p_gen->GetAddressOfReturnLocation());
	}
}

// ---------------------------------------------------------------------------
// Context argument marshalling (Godot -> script function call)
// ---------------------------------------------------------------------------

ArgTemporaries::~ArgTemporaries() {
	for (uint32_t i = 0; i < values.size(); i++) {
		Variant::Type t = values[i].first;
		void *mem = values[i].second;
		if (t == Variant::VARIANT_MAX) {
			static_cast<Variant *>(mem)->~Variant();
		} else {
			builtin_destruct(t, mem);
		}
		memfree(mem);
	}
	values.clear();
}

bool set_context_arg(asIScriptContext *p_ctx, asIScriptFunction *p_func, asUINT p_index,
		const Variant &p_value, ArgTemporaries &p_temporaries) {
	int type_id = 0;
	asDWORD flags = 0;
	if (p_func->GetParam(p_index, &type_id, &flags) < 0) {
		return false;
	}

	if (!(type_id & asTYPEID_MASK_OBJECT)) {
		if (type_id > asTYPEID_DOUBLE) { // enum
			p_ctx->SetArgDWord(p_index, asDWORD(int32_t(p_value.operator int64_t())));
			return true;
		}
		switch (type_id) {
			case asTYPEID_BOOL:
				p_ctx->SetArgByte(p_index, p_value.operator bool() ? 1 : 0);
				return true;
			case asTYPEID_INT8:
			case asTYPEID_UINT8:
				p_ctx->SetArgByte(p_index, asBYTE(p_value.operator int64_t()));
				return true;
			case asTYPEID_INT16:
			case asTYPEID_UINT16:
				p_ctx->SetArgWord(p_index, asWORD(p_value.operator int64_t()));
				return true;
			case asTYPEID_INT32:
			case asTYPEID_UINT32:
				p_ctx->SetArgDWord(p_index, asDWORD(p_value.operator int64_t()));
				return true;
			case asTYPEID_INT64:
			case asTYPEID_UINT64:
				p_ctx->SetArgQWord(p_index, asQWORD(p_value.operator int64_t()));
				return true;
			case asTYPEID_FLOAT:
				p_ctx->SetArgFloat(p_index, float(p_value.operator double()));
				return true;
			case asTYPEID_DOUBLE:
				p_ctx->SetArgDouble(p_index, p_value.operator double());
				return true;
			default:
				return false;
		}
	}

	int base_id = type_id & ~asTYPEID_OBJHANDLE;
	asIScriptEngine *engine = p_ctx->GetEngine();

	if (type_id & asTYPEID_OBJHANDLE) {
		asITypeInfo *type = engine->GetTypeInfoById(base_id);
		Object *obj = p_value.operator Object *();
		void *handle = object_to_as_handle(obj, type);
		p_ctx->SetArgObject(p_index, handle);
		if (handle != nullptr && type != nullptr && (type->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
			// object_to_as_handle returned a strong ref; SetArgObject added its
			// own, so drop ours.
			engine->ReleaseScriptObject(handle, type);
		}
		return true;
	}

	if (is_variant_type_id(base_id)) {
		void *mem = memalloc(sizeof(Variant));
		memnew_placement(mem, Variant(p_value));
		p_temporaries.values.push_back({ Variant::VARIANT_MAX, mem });
		p_ctx->SetArgObject(p_index, mem);
		return true;
	}

	Variant::Type vtype = bridge_get_variant_type(base_id);
	if (vtype != Variant::VARIANT_MAX) {
		void *mem = memalloc(builtin_size(vtype));
		builtin_construct_from_variant(p_value, vtype, mem);
		p_temporaries.values.push_back({ vtype, mem });
		p_ctx->SetArgObject(p_index, mem);
		return true;
	}
	return false;
}

Variant get_context_return(asIScriptContext *p_ctx, asIScriptFunction *p_func) {
	int type_id = p_func->GetReturnTypeId();
	if (type_id == asTYPEID_VOID) {
		return Variant();
	}
	if (!(type_id & asTYPEID_MASK_OBJECT)) {
		return as_to_variant(p_ctx->GetEngine(), p_ctx->GetAddressOfReturnValue(), type_id);
	}
	void *obj = p_ctx->GetReturnObject();
	if (obj == nullptr) {
		return Variant();
	}
	int base_id = type_id & ~asTYPEID_OBJHANDLE;
	if (is_variant_type_id(base_id)) {
		return *static_cast<Variant *>(obj);
	}
	Variant::Type vtype = bridge_get_variant_type(base_id);
	if (vtype != Variant::VARIANT_MAX) {
		return builtin_to_variant(vtype, obj);
	}
	asITypeInfo *type = p_ctx->GetEngine()->GetTypeInfoById(type_id);
	return Variant(as_handle_to_object(obj, type));
}

bool variant_to_as_ref(asIScriptEngine *p_engine, const Variant &p_value, void *p_ref, int p_type_id) {
	if (p_ref == nullptr) {
		return false;
	}
	if (!(p_type_id & asTYPEID_MASK_OBJECT)) {
		if (p_type_id > asTYPEID_DOUBLE) { // enum
			*static_cast<int32_t *>(p_ref) = int32_t(p_value.operator int64_t());
			return true;
		}
		return variant_to_primitive(p_value, p_ref, p_type_id);
	}

	int base_id = p_type_id & ~asTYPEID_OBJHANDLE;
	if (p_type_id & asTYPEID_OBJHANDLE) {
		asITypeInfo *type = p_engine->GetTypeInfoById(base_id);
		void **slot = static_cast<void **>(p_ref);
		Object *obj = p_value.operator Object *();
		void *handle = obj != nullptr ? object_to_as_handle(obj, type) : nullptr;
		bool is_script = type != nullptr && (type->GetFlags() & asOBJ_SCRIPT_OBJECT);
		if (is_script && *slot != nullptr) {
			p_engine->ReleaseScriptObject(*slot, type);
		}
		*slot = handle; // script handles arrive with a ref we keep in the slot
		return true;
	}

	if (is_variant_type_id(base_id)) {
		*static_cast<Variant *>(p_ref) = p_value;
		return true;
	}
	Variant::Type vtype = bridge_get_variant_type(base_id);
	if (vtype != Variant::VARIANT_MAX) {
		// Assign over the existing value via a converted temporary.
		uint8_t buf[sizeof(Variant) * 4];
		CRASH_COND(builtin_size(vtype) > sizeof(buf));
		builtin_construct_from_variant(p_value, vtype, buf);
		builtin_assign(vtype, p_ref, buf);
		builtin_destruct(vtype, buf);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Declaration helpers
// ---------------------------------------------------------------------------

String sanitize_identifier(const String &p_name) {
	static const char *reserved[] = {
		"and", "abstract", "auto", "bool", "break", "case", "cast", "catch", "class",
		"const", "continue", "default", "do", "double", "else", "enum", "explicit",
		"external", "false", "final", "float", "for", "foreach", "from", "funcdef",
		"function", "get", "if", "import", "in", "inout", "int", "interface", "int8",
		"int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or",
		"out", "override", "private", "property", "protected", "return", "set",
		"shared", "super", "switch", "this", "true", "try", "typedef", "uint",
		"uint8", "uint16", "uint32", "uint64", "void", "while", "xor", nullptr
	};
	for (int i = 0; reserved[i] != nullptr; i++) {
		if (p_name == reserved[i]) {
			return p_name + String("_");
		}
	}
	return p_name;
}

String as_type_from_godot_name(const String &p_name) {
	if (p_name.is_empty() || p_name == "Nil") {
		return "Variant";
	}
	if (p_name == "bool") {
		return "bool";
	}
	if (p_name == "int") {
		return "int64";
	}
	if (p_name == "float") {
		return "double";
	}
	if (p_name == "Variant") {
		return "Variant";
	}
	if (p_name.begins_with("enum::") || p_name.begins_with("bitfield::")) {
		return "int64";
	}
	if (p_name.begins_with("typedarray::")) {
		return "Array";
	}
	if (p_name.begins_with("typeddictionary::")) {
		return "Dictionary";
	}
	if (builtin_type_from_name(p_name) != Variant::VARIANT_MAX) {
		return p_name;
	}
	// Assume an Object-derived class.
	return "godot::" + p_name + "@";
}

String as_type_from_property(const Dictionary &p_property_info) {
	Variant::Type type = Variant::Type(int(p_property_info.get("type", 0)));
	int usage = p_property_info.get("usage", 0);
	if (type == Variant::NIL) {
		return "Variant";
	}
	if (type == Variant::OBJECT) {
		String cls = p_property_info.get("class_name", "");
		// class_name may carry hint lists ("Texture2D,-AtlasTexture,...") or
		// enum-ish names ("Node.ProcessMode"); it may also reference classes
		// absent from the runtime ClassDB (editor-only). Fall back to Object.
		cls = cls.get_slice(",", 0);
		if (cls.is_empty() || cls.contains(".") || !is_native_class(StringName(cls))) {
			cls = "Object";
		}
		return "godot::" + cls + "@";
	}
	if (type == Variant::INT) {
		return "int64";
	}
	if (type == Variant::FLOAT) {
		return "double";
	}
	if (type == Variant::BOOL) {
		return "bool";
	}
	switch (type) {
#define X(ENUM_NAME, CPP_TYPE, AS_NAME) \
	case Variant::ENUM_NAME:            \
		return AS_NAME;
		BUILTIN_VALUE_TYPES(X)
#undef X
		default:
			return "Variant";
	}
	(void)usage;
}

String as_param_decl(const String &p_as_type) {
	if (p_as_type == "bool" || p_as_type == "int64" || p_as_type == "double" || p_as_type.ends_with("@")) {
		return p_as_type;
	}
	return "const " + p_as_type + " &in";
}

} // namespace gdas
