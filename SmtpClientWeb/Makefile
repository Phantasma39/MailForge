CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
TARGET   := bridge

.PHONY: all clean run

all: $(TARGET)

$(TARGET): bridge.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) bridge.cpp

clean:
	rm -f $(TARGET)

run: all
	./$(TARGET)
