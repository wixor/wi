CC := gcc
CXX := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64 -DNDEBUG
CXXFLAGS := -O3 -g -pthread -march=native -Wall
LDFLAGS := -pthread -ltalloc -lrt -lstdc++

all: search dump-word corpus-test dedigitize digitize invert lemmatize make-aliases make-binmorfo make-corpus make-index make-mosquare mark-stop

search: search.o common.o
dump-word: dump-word.o common.o 

mark-stop: mark-stop.o common.o
corpus-test: corpus-test.o corpus.o common.o 
dedigitize: dedigitize.o corpus.o common.o
digitize: digitize.o corpus.o common.o
invert: invert.o 
lemmatize: lemmatize.o common.o
make-aliases: make-aliases.o common.o
make-binmorfo: make-binmorfo.o corpus.o common.o
make-corpus: make-corpus.o corpus.o common.o
make-index: make-index.o corpus.o common.o
make-mosquare: make-mosquare.o common.o

common.o: term.o fileio.o bufrw.o
	$(LD) -r $^ -o $@

clean:
	rm -f *.o search dump-word mark-stop corpus-test dedigitize digitize invert lemmatize make-aliases make-binmorfo make-corpus make-index make-mosquare mark-stop

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
