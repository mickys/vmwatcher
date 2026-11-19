# VM Watcher core. No third-party build dependencies: libvirt and zlib are
# loaded at run time. Builds with GCC 10+ or Clang 12+.
CXX      ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -pthread -MMD -MP
LDLIBS   += -pthread -ldl
BUILD    ?= build

SOURCES := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
OBJECTS := $(SOURCES:src/%.cpp=$(BUILD)/%.o)

all: $(BUILD)/vmwatcher

$(BUILD)/vmwatcher: $(OBJECTS) $(BUILD)/main.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/vmwatcher-tests: $(OBJECTS) $(BUILD)/tests.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/tests.o: tests/tests.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/vmwatcher-tests
	$(BUILD)/vmwatcher-tests

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
-include $(wildcard $(BUILD)/*.d)
