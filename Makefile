# Attach to running instance.
# lldb -p $(pgrep mistral-cli)

all:
	cmake --build build -j

test: all
	./build/tests

.PHONY: all test