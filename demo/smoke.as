// Headless smoke test. Run with:
//   godot --headless --path demo res://smoke.tscn
// Exits 0 on success, 1 on failure.
class Smoke : Node {
	[signal] void test_signal(int value) {}

	int failures = 0;
	int received = 0;

	void check(bool condition, const String &in what) {
		if (!condition) {
			failures++;
			push_error("FAIL: " + what);
		} else {
			print("PASS: " + what);
		}
	}

	void _ready() {
		// Builtin value types.
		Vector2 v = Vector2(3, 4);
		check(v.length() == 5.0, "Vector2.length");
		check(v.x == 3.0, "Vector2 member read");
		v.x = 10;
		check(v.x == 10.0, "Vector2 member write");

		String s = "Hello AngelScript";
		check(s.to_upper() == "HELLO ANGELSCRIPT", "String.to_upper");
		check(s.contains("Angel"), "String.contains");

		Array arr;
		arr.push_back(Variant(1));
		arr.push_back(Variant("two"));
		check(arr.size() == 2, "Array.push_back/size");
		check(int64(arr[0]) == 1, "Array indexing");

		Dictionary dict;
		dict[Variant("key")] = Variant(42);
		check(int64(dict[Variant("key")]) == 42, "Dictionary indexing");

		// Utility functions.
		check(absf(-3.5) == 3.5, "absf utility");
		check(clampi(15, 0, 10) == 10, "clampi utility");

		// Node hierarchy through the wrapper layer.
		Node@ child = Node();
		child.set_name("Child");
		add_child(child);
		check(get_child_count() == 1, "add_child/get_child_count");
		Node@ fetched = get_node(NodePath("Child"));
		check(fetched !is null, "get_node returns wrapper");
		check(fetched == child, "wrapper identity equality");

		// Signals declared with [signal] metadata.
		godot::__signal(get_native(), StringName("test_signal"))
				.connect(Callable(get_native(), StringName("on_test_signal")));
		emit_signal(StringName("test_signal"), Variant(42));
		check(received == 42, "script signal emit/connect");

		// Engine singleton access.
		check(Engine.get_frames_per_second() >= 0.0, "Engine singleton");

		if (failures == 0) {
			print("SMOKE TEST PASSED");
		} else {
			print("SMOKE TEST FAILED: " + str(failures) + " failures");
		}
		get_tree().quit(failures == 0 ? 0 : 1);
	}

	void on_test_signal(int value) {
		received = value;
	}
}
