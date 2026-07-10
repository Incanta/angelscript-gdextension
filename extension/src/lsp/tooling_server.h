#ifndef TOOLING_SERVER_H
#define TOOLING_SERVER_H

#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace gdas {

// TCP server for the VS Code language server + debug adapter. One JSON object
// per line; see docs/PROTOCOL.md.
class ToolingServer {
	struct Client {
		godot::Ref<godot::StreamPeerTCP> peer;
		godot::String buffer;
		bool is_debugger = false;
		bool said_hello = false;
	};

	static ToolingServer *singleton;

	godot::Ref<godot::TCPServer> server;
	godot::LocalVector<Client> clients;
	int port = 0;

	void handle_message(Client &p_client, const godot::Dictionary &p_message);
	void send_to(Client &p_client, const godot::Dictionary &p_message);
	void send_type_db(Client &p_client);

public:
	static ToolingServer *get_singleton();
	static void create_singleton();
	static void free_singleton();

	void start();
	void stop();
	// Accepts connections, reads pending lines, dispatches messages. Called
	// every frame and from the debugger's stopped loop.
	void poll();

	void broadcast(const godot::Dictionary &p_message);
	bool has_debug_client() const;
	bool is_listening() const;

	// Pushed by the language after module rebuilds.
	void publish_diagnostics(const godot::String &p_path, const godot::Array &p_items);
	void publish_script_class(const godot::Dictionary &p_class_message);
};

} // namespace gdas

#endif // TOOLING_SERVER_H
