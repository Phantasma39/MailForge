CXX      = g++
CXXFLAGS = -std=c++17 -pthread -Wall -Wextra
TARGET   = mail_server
SRC      = main.cpp src/Server.cpp src/SmtpServer.cpp src/Pop3Server.cpp
INC      = -I include

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(SRC)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
