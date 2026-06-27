CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean

all: tlv_client

tlv_client: tlv_client.cc
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f tlv_client
