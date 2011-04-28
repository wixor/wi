CC := gcc
CXX := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt -lstdc++

all:

search: search.o common.o
make-corpus: make-corpus.o common.o
corpus-test: corpus-test.o common.o
digitize: digitize.o common.o
invert: invert.o
dedigitize: dedigitize.o common.o

common.o: corpus.o term.o fileio.o bufrw.o
	$(LD) -r $^ -o $@

clean:
	rm -f *.o search make-corpus corpus-test digitize invert dedigitize

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
