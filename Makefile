CXX := g++
CC := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt

all:

search: search.o term.o fileio.o bufrw.o
make-corpus: make-corpus.o term.o fileio.o bufrw.o
corpus-test: corpus-test.o corpus.o term.o fileio.o bufrw.o
index-one: index-one.o corpus.o term.o fileio.o bufrw.o

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
