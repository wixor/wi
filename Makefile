CXX := g++
CC := g++
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc

all:

parse-query-test: parse-query-test.o parse-query.o bufrw.o
bufrw-test: bufrw-test.o bufrw.o

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
