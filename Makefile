# Attach to running instance.
# lldb -p $(pgrep mistral-cli)

# nproc is the Linux tool and honours CPU affinity and cgroup limits; getconf
# covers macOS and older glibc; sysctl is the macOS fallback.
#
# Override with e.g. `make JOBS=4` when memory is tight.
JOBS ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

all:
	cmake --build build --parallel $(JOBS)

test: all
	./build/tests

.PHONY: all test