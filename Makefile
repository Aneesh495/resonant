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
	mkdir -p out
	$(BIN) play "$(REFERENCE_PATCH)" 28800 "$(GAIN)" "$(DEVICE)" 48000 128 out/device-soak.csv

.PHONY: workbench
workbench: build
	open "$(BUILD)/Resonant.app" --args "$(abspath $(SESSION))"

.PHONY: patch-gallery data
patch-gallery: build
	python3 tools/gallery.py "$(BIN)" out/gallery

data:
	ls patches/*.rpatch compositions/*.rsn

.PHONY: realtime-audit stress
realtime-audit: build
	$(BUILD)/resonant_campaigns audit

stress: build
	$(BUILD)/resonant_campaigns swap

.PHONY: dsp-check
dsp-check: build
	python3 tools/measure_dsp.py "$(BIN)" out/dsp

PROFILE ?= diagnostic
RUN_ID ?= diagnostic
EVIDENCE ?= evidence/$(RUN_ID)
EXTRA_PROFILE_ARGS ?=
REFERENCE_PATCH ?= patches/reference-64.rpatch
STAGE ?= out/stage

.PHONY: acceptance verify negative-controls bench release
acceptance: build
	python3 tools/evidence.py "$(BUILD)" "$(EVIDENCE)" --profile "$(PROFILE)" $(EXTRA_PROFILE_ARGS)

verify:
	python3 tools/verify_evidence.py "$(EVIDENCE)" --engine "$(BIN)"

negative-controls:
	python3 tools/negative_controls.py "$(EVIDENCE)" "$(BIN)" "$(EVIDENCE).negative-controls.json"

bench: build
	mkdir -p out
	$(BIN) play "$(REFERENCE_PATCH)" "$(SECONDS)" 0 "$(DEVICE)" 48000 128 out/device.csv

release: test demo
	$(CMAKE) --install "$(BUILD)" --prefix "$(STAGE)"

.PHONY: docs
docs:
	python3 tools/check_docs.py
