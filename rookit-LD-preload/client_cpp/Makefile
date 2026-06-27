CXX     ?= g++
CXXFLAGS ?= -Wall -O2 -std=c++11
LDFLAGS ?=

TARGET  = client
SRCS    = client.cpp simple_socket.cpp
HEADERS = simple_socket.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)