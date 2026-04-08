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

BIN = $(BUILD)/resonant
PATCH ?= patches/ember-keys.rpatch
SESSION ?= compositions/ember-motif.rsn
BYTECODE ?= out/patch.rbsc
WAVE ?= out/ember-motif.wav
RATE ?= 48000
BLOCK ?= 128
DEVICE ?= -1
SECONDS ?= 5
GAIN ?= 0

.PHONY: compile-patch verify-bytecode render-audio demo device-smoke device-soak
compile-patch: build
	mkdir -p "$$(dirname "$(BYTECODE)")"
	$(BIN) compile "$(PATCH)" "$(BYTECODE)"

verify-bytecode: build
	$(BIN) verify "$(BYTECODE)"

render-audio: build
	mkdir -p "$$(dirname "$(WAVE)")"
	$(BIN) render "$(SESSION)" "$(WAVE)" "$(RATE)" "$(BLOCK)"

demo: compile-patch render-audio
	$(BIN) verify "$(BYTECODE)"
	$(BIN) inspect "$(PATCH)"
	$(BIN) wave-info "$(WAVE)"

device-smoke: build
	$(BIN) devices
	$(BIN) play "$(PATCH)" "$(SECONDS)" "$(GAIN)" "$(DEVICE)" "$(RATE)" "$(BLOCK)"

device-soak: build
	$(BIN) play "$(PATCH)" 28800 "$(GAIN)" "$(DEVICE)" 48000 128
