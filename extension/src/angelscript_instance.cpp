#include "angelscript_instance.h"

#include "angelscript_language.h"
#include "binding/as_environment.h"
#include "binding/script_bases.h"
#include "binding/variant_bridge.h"

#include <godot_cpp/core/memory.hpp>

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

HashMap<Object *, AngelScriptInstance *> AngelScriptInstance::owner_map;

AngelScriptInstance::AngelScriptInstance(Object *p_owner, const Ref<AngelScriptScript> &p_script,
		asIScriptObject *p_object) {
	owner = p_owner;
	script = p_script;
	object = p_object; // takes over the strong reference
	owner_map.insert(owner, this);
}

AngelScriptInstance::~AngelScriptInstance() {
	owner_map.erase(owner);
	if (object != nullptr) {
		object->Release();
		object = nullptr;
	}
}

asIScriptObject *AngelScriptInstance::lookup_script_object(Object *p_owner) {
	AngelScriptInstance **found = owner_map.getptr(p_owner);
	return found != nullptr ? (*found)->object : nullptr;
}

Variant AngelScriptInstance::call_method(const StringName &p_method, const Variant **p_args,
		int p_argc, GDExtensionCallError &r_error) {
	r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
	if (object == nullptr) {
		return Variant();
	}
	// Only user-declared methods are callable through the script instance;
	// generated forwards must not answer here or native calls that consult the
	// script first (Object::callv) would recurse forever.
	if (!script->get_info().methods.has(p_method)) {
		return Variant();
	}
	asITypeInfo *type = object->GetObjectType();
	asIScriptFunction *func = type->GetMethodByName(String(p_method).utf8().get_data());
	if (func == nullptr) {
		return Variant();
	}
	asUINT param_count = func->GetParamCount();
	if (uint32_t(p_argc) > param_count) {
		r_error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.expected = int32_t(param_count);
		return Variant();
	}
	if (uint32_t(p_argc) < param_count) {
		// AngelScript default arguments are applied at call sites, not here.
		r_error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = int32_t(param_count);
		return Variant();
	}

	AsEnvironment *env = AsEnvironment::get_singleton();
	asIScriptContext *ctx = env->acquire_context();
	ctx->Prepare(func);
	ctx->SetObject(object);
	{
		ArgTemporaries temporaries;
		for (int i = 0; i < p_argc; i++) {
			set_context_arg(ctx, func, asUINT(i), *p_args[i], temporaries);
		}
		env->push_active_context(ctx);
		int r = ctx->Execute();
		env->pop_active_context();
		if (r == asEXECUTION_EXCEPTION) {
			const char *section = nullptr;
			int line = ctx->GetExceptionLineNumber(nullptr, &section);
			ERR_PRINT(vformat("AngelScript exception in %s.%s: %s (%s:%d)",
					String(script->get_info().class_name), String(p_method),
					String::utf8(ctx->GetExceptionString()), String::utf8(section != nullptr ? section : "?"), line));
			env->release_context(ctx);
			r_error.error = GDEXTENSION_CALL_OK;
			return Variant();
		}
		r_error.error = GDEXTENSION_CALL_OK;
		Variant ret = get_context_return(ctx, func);
		env->release_context(ctx);
		return ret;
	}
}

bool AngelScriptInstance::get_property(const StringName &p_name, Variant &r_value) {
	const ScriptClassInfo &info = script->get_info();
	const int *index = info.property_indices.getptr(p_name);
	if (index != nullptr && object != nullptr) {
		int type_id = object->GetPropertyTypeId(asUINT(*index));
		r_value = as_to_variant(object->GetEngine(), object->GetAddressOfProperty(asUINT(*index)), type_id);
		return true;
	}
	if (info.methods.has("_get")) {
		const Variant name_arg = p_name;
		const Variant *args[1] = { &name_arg };
		GDExtensionCallError err;
		Variant ret = call_method("_get", args, 1, err);
		if (err.error == GDEXTENSION_CALL_OK && ret.get_type() != Variant::NIL) {
			r_value = ret;
			return true;
		}
	}
	return false;
}

bool AngelScriptInstance::set_property(const StringName &p_name, const Variant &p_value) {
	const ScriptClassInfo &info = script->get_info();
	const int *index = info.property_indices.getptr(p_name);
	if (index != nullptr && object != nullptr) {
		int type_id = object->GetPropertyTypeId(asUINT(*index));
		return variant_to_as_ref(object->GetEngine(), p_value, object->GetAddressOfProperty(asUINT(*index)), type_id);
	}
	if (info.methods.has("_set")) {
		const Variant name_arg = p_name;
		const Variant *args[2] = { &name_arg, &p_value };
		GDExtensionCallError err;
		Variant ret = call_method("_set", args, 2, err);
		return err.error == GDEXTENSION_CALL_OK && ret.operator bool();
	}
	return false;
}

// ---------------------------------------------------------------------------
// GDExtensionScriptInstanceInfo3 callbacks
// ---------------------------------------------------------------------------

static AngelScriptInstance *as_instance(GDExtensionScriptInstanceDataPtr p_instance) {
	return static_cast<AngelScriptInstance *>(p_instance);
}

static GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	const StringName *name = static_cast<const StringName *>(p_name);
	const Variant *value = static_cast<const Variant *>(p_value);
	return as_instance(p_instance)->set_property(*name, *value);
}

static GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	const StringName *name = static_cast<const StringName *>(p_name);
	Variant *ret = static_cast<Variant *>(r_ret);
	Variant value;
	if (as_instance(p_instance)->get_property(*name, value)) {
		*ret = value;
		return true;
	}
	return false;
}

// PropertyInfo/MethodInfo marshalling helpers (mirrors lua-gdextension).
static void fill_property_info(const Dictionary &p_src, GDExtensionPropertyInfo &r_dst) {
	r_dst.type = GDExtensionVariantType(int(p_src.get("type", 0)));
	r_dst.name = memnew(StringName(p_src.get("name", "")));
	r_dst.class_name = memnew(StringName(p_src.get("class_name", "")));
	r_dst.hint = uint32_t(int(p_src.get("hint", 0)));
	r_dst.hint_string = memnew(String(p_src.get("hint_string", "")));
	r_dst.usage = uint32_t(int(p_src.get("usage", 6))); // default STORAGE|EDITOR
}

static void free_property_info(const GDExtensionPropertyInfo &p_info) {
	memdelete(static_cast<StringName *>(p_info.name));
	memdelete(static_cast<StringName *>(p_info.class_name));
	memdelete(static_cast<String *>(p_info.hint_string));
}

static const GDExtensionPropertyInfo *instance_get_property_list(
		GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const LocalVector<Dictionary> &props = instance->script->get_info().exported_properties;
	*r_count = props.size();
	if (props.is_empty()) {
		return nullptr;
	}
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, props.size());
	for (uint32_t i = 0; i < props.size(); i++) {
		fill_property_info(props[i], list[i]);
	}
	return list;
}

static void instance_free_property_list(GDExtensionScriptInstanceDataPtr p_instance,
		const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	if (p_list == nullptr) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		free_property_info(p_list[i]);
	}
	memdelete_arr(const_cast<GDExtensionPropertyInfo *>(p_list));
}

static GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *name = static_cast<const StringName *>(p_name);
	return instance->script->get_info().property_defaults.has(*name);
}

static GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *name = static_cast<const StringName *>(p_name);
	const Variant *def = instance->script->get_info().property_defaults.getptr(*name);
	if (def == nullptr) {
		return false;
	}
	*static_cast<Variant *>(r_ret) = *def;
	return true;
}

static GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
	return as_instance(p_instance)->owner->_owner;
}

static void instance_get_property_state(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	AngelScriptInstance *instance = as_instance(p_instance);
	for (const Dictionary &prop : instance->script->get_info().exported_properties) {
		StringName name = prop.get("name", "");
		Variant value;
		if (instance->get_property(name, value)) {
			p_add_func(&name, &value, p_userdata);
		}
	}
}

static void fill_method_info(const Dictionary &p_src, GDExtensionMethodInfo &r_dst) {
	r_dst.name = memnew(StringName(p_src.get("name", "")));
	fill_property_info(p_src.get("return", Dictionary()), r_dst.return_value);
	r_dst.flags = uint32_t(int(p_src.get("flags", 1)));
	r_dst.id = int32_t(int(p_src.get("id", 0)));
	Array args = p_src.get("args", Array());
	r_dst.argument_count = uint32_t(args.size());
	r_dst.arguments = nullptr;
	if (r_dst.argument_count > 0) {
		GDExtensionPropertyInfo *arg_list = memnew_arr(GDExtensionPropertyInfo, r_dst.argument_count);
		for (uint32_t i = 0; i < r_dst.argument_count; i++) {
			fill_property_info(args[i], arg_list[i]);
		}
		r_dst.arguments = arg_list;
	}
	r_dst.default_argument_count = 0;
	r_dst.default_arguments = nullptr;
}

static void free_method_info(const GDExtensionMethodInfo &p_info) {
	memdelete(static_cast<StringName *>(p_info.name));
	free_property_info(p_info.return_value);
	if (p_info.arguments != nullptr) {
		for (uint32_t i = 0; i < p_info.argument_count; i++) {
			free_property_info(p_info.arguments[i]);
		}
		memdelete_arr(const_cast<GDExtensionPropertyInfo *>(p_info.arguments));
	}
}

static const GDExtensionMethodInfo *instance_get_method_list(
		GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const HashMap<StringName, Dictionary> &methods = instance->script->get_info().methods;
	*r_count = uint32_t(methods.size());
	if (methods.is_empty()) {
		return nullptr;
	}
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, methods.size());
	uint32_t i = 0;
	for (const KeyValue<StringName, Dictionary> &kv : methods) {
		fill_method_info(kv.value, list[i++]);
	}
	return list;
}

static void instance_free_method_list(GDExtensionScriptInstanceDataPtr p_instance,
		const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	if (p_list == nullptr) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		free_method_info(p_list[i]);
	}
	memdelete_arr(const_cast<GDExtensionMethodInfo *>(p_list));
}

static GDExtensionVariantType instance_get_property_type(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *name = static_cast<const StringName *>(p_name);
	for (const Dictionary &prop : instance->script->get_info().exported_properties) {
		if (StringName(prop.get("name", "")) == *name) {
			*r_is_valid = true;
			return GDExtensionVariantType(int(prop.get("type", 0)));
		}
	}
	*r_is_valid = false;
	return GDEXTENSION_VARIANT_TYPE_NIL;
}

static GDExtensionBool instance_validate_property(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionPropertyInfo *p_property) {
	return false;
}

static GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *name = static_cast<const StringName *>(p_name);
	return instance->script->get_info().methods.has(*name);
}

static GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *name = static_cast<const StringName *>(p_name);
	const Dictionary *method = instance->script->get_info().methods.getptr(*name);
	if (method == nullptr) {
		*r_is_valid = false;
		return 0;
	}
	*r_is_valid = true;
	return Array(method->get("args", Array())).size();
}

static void instance_call(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args,
		GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	AngelScriptInstance *instance = as_instance(p_instance);
	const StringName *method = static_cast<const StringName *>(p_method);
	const Variant **args = reinterpret_cast<const Variant **>(const_cast<GDExtensionConstVariantPtr *>(p_args));
	Variant ret = instance->call_method(*method, args, int(p_argument_count), *r_error);
	*static_cast<Variant *>(r_return) = ret;
}

static void instance_notification(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what,
		GDExtensionBool p_reversed) {
	AngelScriptInstance *instance = as_instance(p_instance);
	if (!instance->script->get_info().methods.has("_notification")) {
		return;
	}
	const Variant what = int64_t(p_what);
	const Variant *args[1] = { &what };
	GDExtensionCallError err;
	instance->call_method("_notification", args, 1, err);
}

static void instance_to_string(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
	AngelScriptInstance *instance = as_instance(p_instance);
	if (!instance->script->get_info().methods.has("_to_string")) {
		*r_is_valid = false;
		return;
	}
	GDExtensionCallError err;
	Variant ret = instance->call_method("_to_string", nullptr, 0, err);
	if (err.error != GDEXTENSION_CALL_OK) {
		*r_is_valid = false;
		return;
	}
	*static_cast<String *>(r_out) = ret.operator String();
	*r_is_valid = true;
}

static void instance_refcount_incremented(GDExtensionScriptInstanceDataPtr p_instance) {}

static GDExtensionBool instance_refcount_decremented(GDExtensionScriptInstanceDataPtr p_instance) {
	return true; // no cycles through the script object; the owner may die
}

static GDExtensionObjectPtr instance_get_script(GDExtensionScriptInstanceDataPtr p_instance) {
	return as_instance(p_instance)->script->_owner;
}

static GDExtensionBool instance_is_placeholder(GDExtensionScriptInstanceDataPtr p_instance) {
	return false;
}

static GDExtensionBool instance_set_fallback(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	return false;
}

static GDExtensionBool instance_get_fallback(GDExtensionScriptInstanceDataPtr p_instance,
		GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	return false;
}

static GDExtensionScriptLanguagePtr instance_get_language(GDExtensionScriptInstanceDataPtr p_instance);

static void instance_free(GDExtensionScriptInstanceDataPtr p_instance) {
	memdelete(as_instance(p_instance));
}

static GDExtensionScriptInstanceInfo3 instance_info = {
	&instance_set, // set_func
	&instance_get, // get_func
	&instance_get_property_list, // get_property_list_func
	&instance_free_property_list, // free_property_list_func
	nullptr, // get_class_category_func
	&instance_property_can_revert, // property_can_revert_func
	&instance_property_get_revert, // property_get_revert_func
	&instance_get_owner, // get_owner_func
	&instance_get_property_state, // get_property_state_func
	&instance_get_method_list, // get_method_list_func
	&instance_free_method_list, // free_method_list_func
	&instance_get_property_type, // get_property_type_func
	&instance_validate_property, // validate_property_func
	&instance_has_method, // has_method_func
	&instance_get_method_argument_count, // get_method_argument_count_func
	&instance_call, // call_func
	&instance_notification, // notification_func
	&instance_to_string, // to_string_func
	&instance_refcount_incremented, // refcount_incremented_func
	&instance_refcount_decremented, // refcount_decremented_func
	&instance_get_script, // get_script_func
	&instance_is_placeholder, // is_placeholder_func
	&instance_set_fallback, // set_fallback_func
	&instance_get_fallback, // get_fallback_func
	&instance_get_language, // get_language_func
	&instance_free, // free_func
};

const GDExtensionScriptInstanceInfo3 *AngelScriptInstance::get_instance_info() {
	return &instance_info;
}

static GDExtensionScriptLanguagePtr instance_get_language(GDExtensionScriptInstanceDataPtr p_instance) {
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	return lang != nullptr ? GDExtensionScriptLanguagePtr(lang->_owner) : nullptr;
}

} // namespace gdas
