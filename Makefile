CXX := g++
CC := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt

all:

search: search.o term.o bufrw.o
bufrw-test: bufrw-test.o bufrw.o

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
