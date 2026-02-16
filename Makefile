BUILD ?= build
CMAKE ?= cmake
CONFIG ?= Release
JOBS ?= 4

.PHONY: bootstrap doctor build test
bootstrap:
	$(CMAKE) -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(CONFIG) $(CMAKE_ARGS)

doctor:
	uname -sm
	$(CMAKE) --version
	$(CXX) --version
	df -h .
	@if command -v pkg-config >/dev/null; then pkg-config --modversion portaudio-2.0 || true; fi

build: bootstrap
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure
