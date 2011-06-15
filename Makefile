CC := gcc
CXX := g++
CPPFLAGS := -D_FILE_OFFSET_BITS=64 
CXXFLAGS := -O3 -g -pthread -march=native -Wall -msse -msse2 -mfpmath=sse -ffast-math
LDFLAGS := -pthread -ltalloc -lrt -lstdc++ -lm

DATA_WIKI := wiki_dane/wikipedia_dla_wyszukiwarek.txt
DATA_MORFO := wiki_dane/bazy_do_tm.txt
DATA_STOP := wiki_dane/stop_words.txt 
DATA_INTERESTING := wiki_dane/interesujace_artykuly.txt
DATA_LINKS := wiki_dane/wikilinki.txt

all: search dump-word corpus-test dedigitize digitize invert lemmatize make-aliases make-binmorfo make-corpus make-index make-mosquare make-pagerank mark-stop

search: search.o common.o
dump-word: dump-word.o common.o 

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
make-pagerank: make-pagerank.o common.o
mark-stop: mark-stop.o common.o

common.o: term.o fileio.o bufrw.o
	$(LD) -r $^ -o $@

clean:
	rm -f *.o search dump-word mark-stop corpus-test dedigitize digitize invert lemmatize make-aliases make-binmorfo make-corpus make-index make-mosquare make-pagerank mark-stop

db/tokenized: tokenize.py $(DATA_WIKI) $(DATA_INTERESTING)
	./tokenize.py $(DATA_WIKI) $(DATA_INTERESTING) >/dev/null
db/wikilinks: wikilinks_graph.py db/tokenized $(DATA_LINKS)
	./wikilinks_graph.py $(DATA_LINKS) >/dev/null
db/corpus: make-corpus db/tokenized $(DATA_MORFO)
	./make-corpus db/tokenized $(DATA_MORFO)
db/morfologik: make-binmorfo db/corpus $(DATA_MORFO)
	./make-binmorfo $(DATA_MORFO)
db/aliases: make-aliases db/morfologik
	./make-aliases
db/mosquare: make-mosquare db/morfologik db/aliases
	./make-mosquare
db/inverted: digitize invert db/corpus db/tokenized
	./digitize <db/tokenized >db/digital; \
    ./invert db/digital >db/inverted; \
    rm db/digital
db/invlemma: lemmatize invert db/inverted db/mosquare
	./lemmatize <db/inverted >db/lemmatized; \
	./invert db/lemmatized >db/invlemma; \
    rm db/lemmatized
the-index: make-index make-pagerank mark-stop db/wikilinks db/inverted db/invlemma $(DATA_STOP)
	./make-index; \
 	./make-pagerank; \
    ./mark-stop < $(DATA_STOP);

Makefile.deps: $(wildcard *.h *.c *.cpp)
	$(CXX) -MM $(filter %.c %.cpp, $^) > $@
include Makefile.deps
