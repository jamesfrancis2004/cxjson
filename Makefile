PYTHON ?= python3

.PHONY: all build wheel test pgo clean

all: build

# In-place build for fast iterative development
build:
	$(PYTHON) setup.py build_ext --inplace

# Profile-Guided Optimization build (clang, native arch)
pgo:
	ARCHFLAGS="-arch $(shell uname -m)" CFLAGS="-arch $(shell uname -m) -fprofile-instr-generate=/tmp/cxjson-%p.profraw" LDFLAGS="-arch $(shell uname -m) -fprofile-instr-generate" $(PYTHON) setup.py build_ext --inplace --force
	PYTHONPATH=src $(PYTHON) tests/bench_comprehensive.py > /dev/null
	xcrun llvm-profdata merge -output=/tmp/cxjson.profdata /tmp/cxjson-*.profraw
	ARCHFLAGS="-arch $(shell uname -m)" CFLAGS="-arch $(shell uname -m) -fprofile-instr-use=/tmp/cxjson.profdata" LDFLAGS="-arch $(shell uname -m)" $(PYTHON) setup.py build_ext --inplace --force
	rm -f /tmp/cxjson-*.profraw /tmp/cxjson.profdata

# Build distributable wheel (.whl) into dist/
wheel:
	$(PYTHON) -m pip wheel . --no-deps -w dist/

# Run the test suite
test: build
	PYTHONPATH=src $(PYTHON) tests/test_serializer.py
	PYTHONPATH=src $(PYTHON) tests/test_orjson_compatibility.py
	PYTHONPATH=src $(PYTHON) tests/test_conformance_fuzz.py

# Clean build and distribution artifacts
clean:
	rm -rf build/ dist/ *.egg-info src/*.egg-info src/cxjson/*.so src/cxjson/*.dylib
	find . -type d -name "__pycache__" -exec rm -rf {} +
