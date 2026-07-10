import os

env = SConscript("extension/godot-cpp/SConstruct").Clone()

# Setup variant build dir for each configuration
build_dir = f"build/{env['suffix'][1:]}"
env["build_dir"] = build_dir
VariantDir(build_dir, "extension/src", duplicate=False)

# Compilation database for clangd / editors
env.Tool("compilation_db")
compiledb = env.CompilationDatabase("compile_commands.json")
env.Alias("compiledb", compiledb)


def remove_options(flags, *options):
    removed = False
    for opt in options:
        if opt in flags:
            flags.remove(opt)
            removed = True
    return removed


# AngelScript uses exceptions in a few places; keep them enabled
remove_options(env["CXXFLAGS"], "-fno-exceptions")
if env["platform"] == "windows" and not env.get("use_mingw"):
    env.Append(CXXFLAGS=["/EHsc"])

# Don't strip the entry symbol
remove_options(env["LINKFLAGS"], "-s")

# Build the AngelScript runtime + the add-ons we use
env.Tool("angelscript", toolpath=["tools"])

# Generate builtin type binding tables from extension_api.json
generated = env.Command(
    "extension/src/binding/builtin_bindings.gen.cpp",
    ["extension/godot-cpp/gdextension/extension_api.json", "tools/gen_bindings.py"],
    "python3 tools/gen_bindings.py extension/godot-cpp/gdextension/extension_api.json $TARGET",
)

source_directories = [".", "binding", "debugger", "lsp"]
sources = [
    Glob(f"{build_dir}/{directory}/*.cpp")
    for directory in source_directories
]
gen_target = f"{build_dir}/binding/builtin_bindings.gen.cpp"
if not any(str(node).endswith("builtin_bindings.gen.cpp") for group in sources for node in group):
    sources.append(env.File(gen_target))

if env["platform"] == "ios":
    library = env.StaticLibrary(
        f"addons/godot-angelscript/bin/libgodot-angelscript{env['suffix']}{env['LIBSUFFIX']}",
        source=sources,
    )
else:
    library = env.SharedLibrary(
        f"addons/godot-angelscript/bin/libgodot-angelscript{env['suffix']}{env['SHLIBSUFFIX']}",
        source=sources,
    )
Default(library)
