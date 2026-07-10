class Main : Node2D {
	[export] String greeting = "Hello from AngelScript!";

	int frames = 0;

	void _ready() {
		print(greeting);
	}

	void _process(double delta) {
		frames++;
		if (frames == 5) {
			print("Five frames rendered, last delta: " + str(delta));
		}
	}
}
