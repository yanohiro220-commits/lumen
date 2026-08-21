BUILD ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4))

.PHONY: all build test bench asan repl clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD) -j$(JOBS)

test: build
	./$(BUILD)/lumen_tests

bench: build
	./$(BUILD)/lumen_bench

asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLUMEN_SANITIZE=ON
	cmake --build build-asan -j$(JOBS)
	./build-asan/lumen_tests

repl: build
	./$(BUILD)/lumen

examples: build
	@for f in examples/*.lum; do echo "== $$f"; ./$(BUILD)/lumen $$f; done

clean:
	rm -rf $(BUILD) build-asan
