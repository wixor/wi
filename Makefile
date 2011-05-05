CC := gcc
CXX := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt -lstdc++

all:

search: search.o common.o
make-stop: make-stop.o common.o

common.o: term.o fileio.o bufrw.o
	$(LD) -r $^ -o $@

clean:
	rm -f *.o search make-stop

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
