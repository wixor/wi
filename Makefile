CXX := g++
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread

all:

bufrw-test: bufrw-test.o bufrw.o

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
