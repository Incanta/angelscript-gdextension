#include "tooling_server.h"

#include "../angelscript_language.h"
#include "../binding/native_classes.h"
#include "../binding/variant_bridge.h"
#include "../debugger/as_debugger.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace gdas {

ToolingServer *ToolingServer::singleton = nullptr;

ToolingServer *ToolingServer::get_singleton() {
	return singleton;
}

void ToolingServer::create_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(ToolingServer);
		singleton->start();
	}
}

void ToolingServer::free_singleton() {
	if (singleton != nullptr) {
		singleton->stop();
		memdelete(singleton);
		singleton = nullptr;
	}
}

void ToolingServer::start() {
	int base_port = 27500;
	String env_port = OS::get_singleton()->get_environment("GDAS_TOOLING_PORT");
	if (!env_port.is_empty() && env_port.is_valid_int()) {
		base_port = int(env_port.to_int());
	} else if (ProjectSettings::get_singleton()->has_setting("angelscript/tooling/port")) {
		base_port = int(ProjectSettings::get_singleton()->get_setting("angelscript/tooling/port", 27500));
	}

	server.instantiate();
	for (int candidate = base_port; candidate < base_port + 10; candidate++) {
		if (server->listen(candidate, "127.0.0.1") == OK) {
			port = candidate;
			UtilityFunctions::print_verbose(vformat("AngelScript: tooling server listening on 127.0.0.1:%d", port));
			return;
		}
	}
	WARN_PRINT(vformat("AngelScript: tooling server could not listen on ports %d-%d.", base_port, base_port + 9));
	server.unref();
}

void ToolingServer::stop() {
	clients.clear();
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
}

bool ToolingServer::is_listening() const {
	return server.is_valid() && server->is_listening();
}

bool ToolingServer::has_debug_client() const {
	for (const Client &client : clients) {
		if (client.is_debugger) {
			return true;
		}
	}
	return false;
}

void ToolingServer::poll() {
	if (!is_listening()) {
		return;
	}
	while (server->is_connection_available()) {
		Client client;
		client.peer = server->take_connection();
		clients.push_back(client);
	}

	for (int64_t i = int64_t(clients.size()) - 1; i >= 0; i--) {
		Client &client = clients[uint32_t(i)];
		client.peer->poll();
		StreamPeerTCP::Status status = client.peer->get_status();
		if (status != StreamPeerTCP::STATUS_CONNECTED) {
			if (status != StreamPeerTCP::STATUS_CONNECTING) {
				clients.remove_at(uint32_t(i));
			}
			continue;
		}
		int available = client.peer->get_available_bytes();
		if (available > 0) {
			Array result = client.peer->get_data(available);
			if (int(result[0]) == int(OK)) {
				PackedByteArray bytes = result[1];
				client.buffer += String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size());
			}
		}
		int newline = client.buffer.find("\n");
		while (newline >= 0) {
			String line = client.buffer.substr(0, newline).strip_edges();
			client.buffer = client.buffer.substr(newline + 1);
			if (!line.is_empty()) {
				Variant parsed = JSON::parse_string(line);
				if (parsed.get_type() == Variant::DICTIONARY) {
					handle_message(client, parsed);
				}
			}
			newline = client.buffer.find("\n");
		}
	}
}

void ToolingServer::send_to(Client &p_client, const Dictionary &p_message) {
	String line = JSON::stringify(p_message) + "\n";
	CharString utf8 = line.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	p_client.peer->put_data(bytes);
}

void ToolingServer::broadcast(const Dictionary &p_message) {
	for (Client &client : clients) {
		if (client.said_hello) {
			send_to(client, p_message);
		}
	}
}

void ToolingServer::publish_diagnostics(const String &p_path, const Array &p_items) {
	Dictionary message;
	message["type"] = "diagnostics";
	message["path"] = p_path;
	message["items"] = p_items;
	broadcast(message);
}

void ToolingServer::publish_script_class(const Dictionary &p_class_message) {
	broadcast(p_class_message);
}

// ---------------------------------------------------------------------------
// Type database export
// ---------------------------------------------------------------------------

// Converts an internal PropertyInfo dict to the protocol's AS type spelling.
static String protocol_type(const Dictionary &p_prop) {
	String type = as_type_from_property(p_prop);
	if (type.begins_with("godot::")) {
		return type.trim_prefix("godot::");
	}
	return type;
}

static Dictionary class_spec_to_message(const ClassSpec &p_spec) {
	Dictionary msg;
	msg["type"] = "type_db_class";
	msg["name"] = String(p_spec.name);
	msg["base"] = String(p_spec.parent);
	msg["native"] = true;

	Array methods;
	for (int i = 0; i < p_spec.methods.size(); i++) {
		Dictionary m = p_spec.methods[i];
		int flags = m.get("flags", 0);
		if (flags & METHOD_FLAG_VIRTUAL) {
			continue;
		}
		Dictionary method;
		method["name"] = m.get("name", "");
		Dictionary ret = m.get("return", Dictionary());
		method["return"] = int(ret.get("type", 0)) == 0 ? String("void") : protocol_type(ret);
		method["static"] = bool(flags & METHOD_FLAG_STATIC);
		method["vararg"] = bool(flags & METHOD_FLAG_VARARG);
		Array args;
		Array in_args = m.get("args", Array());
		for (int a = 0; a < in_args.size(); a++) {
			Dictionary in_arg = in_args[a];
			Dictionary arg;
			arg["name"] = in_arg.get("name", "");
			arg["type"] = protocol_type(in_arg);
			arg["default"] = Variant();
			args.push_back(arg);
		}
		method["args"] = args;
		methods.push_back(method);
	}
	msg["methods"] = methods;

	Array properties;
	for (int i = 0; i < p_spec.properties.size(); i++) {
		Dictionary p = p_spec.properties[i];
		String name = p.get("name", "");
		if (name.is_empty() || name.contains("/")) {
			continue;
		}
		Dictionary prop;
		prop["name"] = name;
		prop["type"] = protocol_type(p);
		properties.push_back(prop);
	}
	msg["properties"] = properties;

	Array signals;
	for (int i = 0; i < p_spec.signals.size(); i++) {
		Dictionary s = p_spec.signals[i];
		Dictionary sig;
		sig["name"] = s.get("name", "");
		Array args;
		Array in_args = s.get("args", Array());
		for (int a = 0; a < in_args.size(); a++) {
			Dictionary in_arg = in_args[a];
			Dictionary arg;
			arg["name"] = in_arg.get("name", "");
			arg["type"] = protocol_type(in_arg);
			args.push_back(arg);
		}
		sig["args"] = args;
		signals.push_back(sig);
	}
	msg["signals"] = signals;

	Array constants;
	Array enums;
	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	PackedStringArray enum_list = class_db->class_get_enum_list(p_spec.name, true);
	HashSet<String> enum_constant_names;
	for (const String &enum_name : enum_list) {
		Dictionary en;
		en["name"] = enum_name;
		Array values;
		for (const String &value_name : class_db->class_get_enum_constants(p_spec.name, enum_name, true)) {
			enum_constant_names.insert(value_name);
			Dictionary value;
			value["name"] = value_name;
			value["value"] = class_db->class_get_integer_constant(p_spec.name, value_name);
			values.push_back(value);
		}
		en["values"] = values;
		enums.push_back(en);
	}
	for (const String &const_name : class_db->class_get_integer_constant_list(p_spec.name, true)) {
		if (enum_constant_names.has(const_name)) {
			continue;
		}
		Dictionary constant;
		constant["name"] = const_name;
		constant["value"] = class_db->class_get_integer_constant(p_spec.name, const_name);
		constants.push_back(constant);
	}
	msg["constants"] = constants;
	msg["enums"] = enums;
	return msg;
}

void ToolingServer::send_type_db(Client &p_client) {
	Dictionary begin;
	begin["type"] = "type_db_begin";
	send_to(p_client, begin);

	int count = 0;
	for (const KeyValue<StringName, ClassSpec> &kv : get_class_specs()) {
		send_to(p_client, class_spec_to_message(kv.value));
		count++;
	}

	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	if (lang != nullptr) {
		for (const Dictionary &msg : lang->get_script_class_messages()) {
			send_to(p_client, msg);
			count++;
		}
	}

	Dictionary end;
	end["type"] = "type_db_end";
	end["count"] = count;
	send_to(p_client, end);
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void ToolingServer::handle_message(Client &p_client, const Dictionary &p_message) {
	String type = p_message.get("type", "");

	if (type == "hello") {
		p_client.said_hello = true;
		p_client.is_debugger = String(p_message.get("client", "")).contains("dap");
		Dictionary welcome;
		welcome["type"] = "welcome";
		welcome["engine"] = "godot";
		welcome["godot_version"] = String(Engine::get_singleton()->get_version_info().get("string", ""));
		welcome["extension_version"] = "0.1.0";
		welcome["version"] = 1;
		send_to(p_client, welcome);
		return;
	}
	if (!p_client.said_hello) {
		return;
	}

	AsDebugger *debugger = AsDebugger::get_singleton();

	if (type == "request_type_db") {
		send_type_db(p_client);
	} else if (type == "set_breakpoints") {
		String path = p_message.get("path", "");
		PackedInt32Array lines = p_message.get("lines", PackedInt32Array());
		if (debugger != nullptr) {
			debugger->set_breakpoints(path, lines);
		}
		Dictionary reply;
		reply["type"] = "breakpoints_set";
		reply["path"] = path;
		reply["verified"] = lines;
		send_to(p_client, reply);
	} else if (type == "continue") {
		if (debugger != nullptr) {
			debugger->resume(AsDebugger::ACTION_CONTINUE);
		}
	} else if (type == "pause") {
		if (debugger != nullptr) {
			debugger->request_pause();
		}
	} else if (type == "step_over") {
		if (debugger != nullptr) {
			debugger->resume(AsDebugger::ACTION_STEP_OVER);
		}
	} else if (type == "step_in") {
		if (debugger != nullptr) {
			debugger->resume(AsDebugger::ACTION_STEP_IN);
		}
	} else if (type == "step_out") {
		if (debugger != nullptr) {
			debugger->resume(AsDebugger::ACTION_STEP_OUT);
		}
	} else if (type == "request_stack") {
		Dictionary reply;
		reply["type"] = "stack";
		reply["frames"] = debugger != nullptr ? debugger->get_stack_frames() : Array();
		send_to(p_client, reply);
	} else if (type == "request_variables") {
		int frame = p_message.get("frame", 0);
		String scope = p_message.get("scope", "locals");
		Dictionary reply;
		reply["type"] = "variables";
		reply["frame"] = frame;
		reply["scope"] = scope;
		reply["variables"] = debugger != nullptr ? debugger->get_variables(frame, scope) : Array();
		send_to(p_client, reply);
	} else if (type == "request_evaluate") {
		int frame = p_message.get("frame", 0);
		String expression = p_message.get("expression", "");
		Dictionary reply;
		reply["type"] = "evaluate";
		if (debugger != nullptr) {
			Dictionary result = debugger->evaluate(frame, expression);
			reply["value"] = result.get("value", "");
			reply["value_type"] = result.get("value_type", "");
		}
		send_to(p_client, reply);
	}
}

} // namespace gdas
