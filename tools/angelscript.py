"""SCons tool that builds the AngelScript SDK and the add-ons we use."""

import os

SDK_DIR = "extension/angelscript/sdk"
ADDONS = ["scriptbuilder", "scripthandle", "scripthelper", "weakref", "datetime"]


def exists(env):
    return os.path.isdir(SDK_DIR)


def generate(env):
    env.Append(CPPPATH=[
        f"{SDK_DIR}/angelscript/include",
        f"{SDK_DIR}/add_on",
    ])

    sources = env.Glob(f"{SDK_DIR}/angelscript/source/*.cpp")

    # Platform-specific assembly for native calling conventions
    platform = env["platform"]
    arch = env["arch"]
    if platform == "web":
        # No native calling conventions in WASM
        env.Append(CPPDEFINES=["AS_MAX_PORTABILITY"])
    elif arch == "arm64":
        if platform == "windows":
            sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_arm64_msvc.asm")
        elif platform in ("macos", "ios"):
            sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_arm64_xcode.S")
        else:
            sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_arm64_gcc.S")
    elif arch == "arm32":
        if platform == "windows":
            sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_arm_msvc.asm")
        else:
            sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_arm_gcc.S")
    elif arch == "x86_64" and platform == "windows" and not env.get("use_mingw"):
        sources.append(f"{SDK_DIR}/angelscript/source/as_callfunc_x64_msvc_asm.asm")

    if platform == "windows" and not env.get("use_mingw"):
        env.Append(CPPDEFINES=["ANGELSCRIPT_EXPORT", "_CRT_SECURE_NO_WARNINGS"])
        # Enable MASM/armasm for the .asm files
        if arch == "arm64":
            env.Tool("masm")
        else:
            env.Tool("masm")

    for addon in ADDONS:
        sources.extend(env.Glob(f"{SDK_DIR}/add_on/{addon}/*.cpp"))

    lib = env.StaticLibrary(
        f"build/angelscript{env['suffix']}",
        source=sources,
    )
    env.Append(LIBS=[lib])
