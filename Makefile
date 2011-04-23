CXX := g++
CC := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt

COMMON := corpus.o term.o fileio.o bufrw.o

all:

search: search.o $(COMMON)
make-corpus: make-corpus.o $(COMMON)
corpus-test: corpus-test.o $(COMMON)
digitize: digitize.o $(COMMON)
invert: invert.o
dedigitize: dedigitize.o $(COMMON)

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
