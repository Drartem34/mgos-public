CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -s
GTK_FLAGS := $(shell pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)

runner: runner.cpp
	$(CXX) $(CXXFLAGS) runner.cpp -o runner $(GTK_FLAGS)

clean:
	rm -f runner

.PHONY: clean
