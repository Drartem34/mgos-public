CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -s
GTK_FLAGS := $(shell pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)
LIBS := $(GTK_FLAGS) -lyyjson

TARGET := dock
SRC := dock.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
