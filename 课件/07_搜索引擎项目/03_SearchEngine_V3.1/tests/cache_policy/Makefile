CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
ROOT := ../..
CACHE_SOURCES := \
	$(ROOT)/src/cache/TinyLfuFrequencySketch.cc \
	$(ROOT)/src/cache/ShardedWTinyLfuCache.cc

.PHONY: all test benchmark clean

all: cache_policy_test cache_policy_benchmark

cache_policy_test: cache_policy_test.cc $(CACHE_SOURCES)
	$(CXX) $(CXXFLAGS) -pthread $^ -o $@

cache_policy_benchmark: cache_policy_benchmark.cc \
	$(ROOT)/src/cache/ShardedLruCache.cc $(CACHE_SOURCES)
	$(CXX) $(CXXFLAGS) -pthread $^ -o $@

test: cache_policy_test
	./cache_policy_test

benchmark: cache_policy_benchmark
	./cache_policy_benchmark

clean:
	rm -f cache_policy_test cache_policy_benchmark
