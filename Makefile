BUILD ?= build

.PHONY: all test clean

all:
	cmake -S . -B $(BUILD) -DCMAKE_CXX_COMPILER=g++
	cmake --build $(BUILD) -j

test: all
	./$(BUILD)/ps5ai-11dim --self-test

clean:
	rm -rf $(BUILD)
