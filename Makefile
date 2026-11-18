CXX := g++
CXXFLAGS := -O2 -std=c++20 -Wall -Wextra -Werror -pedantic -Icpp/include
LDFLAGS :=

BUILD_DIR := build
TARGET := nitrosense-btop
TEST_TARGET := $(BUILD_DIR)/test_runner
PREFIX ?= /usr/local

CORE_SRCS := cpp/src/nitro_types.cpp cpp/src/sensors.cpp cpp/src/control.cpp
CORE_OBJS := $(patsubst cpp/src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))
APP_OBJS := $(CORE_OBJS) $(BUILD_DIR)/tui.o $(BUILD_DIR)/main.o
TEST_OBJS := $(CORE_OBJS) $(BUILD_DIR)/tui.o $(BUILD_DIR)/test_main.o

.PHONY: all test install uninstall clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: cpp/src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_main.o: cpp/tests/test_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(APP_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
