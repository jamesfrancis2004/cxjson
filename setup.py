import sys
from setuptools import Extension, find_packages, setup

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

c_extension = Extension(
    name="cxjson._cxjson",
    sources=[
        "src/c/_cxjson_module.c",
        "src/c/cjson_simd.c",
        "src/c/serializer.c",
        "src/c/deserializer.c",
        "src/c/zmij/zmij.c",
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

