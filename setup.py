import os
import sys
from setuptools import Extension, find_packages, setup

pgo_mode = os.environ.get("CXJSON_PGO", "").strip().lower()
pgo_dir = os.environ.get("CXJSON_PGO_DIR", "/tmp/cxjson_pgo")

if sys.platform == "win32":
    extra_compile_args = ["/O2", "/W3"]
    extra_link_args = []
else:
    extra_compile_args = [
        "-O3",
        "-Wall",
        "-Wno-unused-function",
        "-flto",
        "-fvisibility=hidden",
        "-fomit-frame-pointer",
        "-fno-math-errno",
        "-fno-trapping-math",
        "-fstrict-aliasing",
        "-funroll-loops",
    ]
    extra_link_args = ["-flto"]
    if sys.platform == "darwin":
        extra_link_args.append("-Wl,-dead_strip")
        if pgo_mode == "generate":
            os.makedirs(pgo_dir, exist_ok=True)
            profraw = os.path.join(pgo_dir, "cxjson.profraw")
            extra_compile_args.append(f"-fprofile-instr-generate={profraw}")
            extra_link_args.append(f"-fprofile-instr-generate={profraw}")
        elif pgo_mode == "use":
            profdata = os.path.join(pgo_dir, "cxjson.profdata")
            if os.path.exists(profdata):
                extra_compile_args.append(f"-fprofile-instr-use={profdata}")
                extra_link_args.append(f"-fprofile-instr-use={profdata}")
    else:
        if pgo_mode == "generate":
            os.makedirs(pgo_dir, exist_ok=True)
            extra_compile_args.append(f"-fprofile-generate={pgo_dir}")
            extra_link_args.append(f"-fprofile-generate={pgo_dir}")
        elif pgo_mode == "use" and os.path.isdir(pgo_dir):
            extra_compile_args.extend([f"-fprofile-use={pgo_dir}", "-fprofile-correction"])
            extra_link_args.append(f"-fprofile-use={pgo_dir}")

c_extension = Extension(
    name="cxjson._cxjson",
    sources=[
        "src/c/_cxjson_module.c",
        "src/c/cjson_simd.c",
        "src/c/serializer.c",
        "src/c/deserializer.c",
        "src/c/zmij/zmij.c",
        "src/c/zmij/zmij_sse41.c",
    ],
    include_dirs=["src/c", "src/c/zmij", "src/c/jeaiii"],
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

setup(
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    package_data={"cxjson": ["py.typed", "__init__.pyi"]},
    ext_modules=[c_extension],
)

