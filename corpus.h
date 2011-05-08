#ifndef __CORPUS_H__
#define __CORPUS_H__

#include "term.h"

class Corpus
{
    TermHasher th;
    void *memctx;
    int *bucket_offsets;
    const char **terms;

public:
    Corpus();
    Corpus(const char *filename);
    ~Corpus();

    void read(const char *filename);
    int lookup(const char *text, size_t len) const;
    void lookup(int idx, char *out, size_t len) const;
    int term_length(int idx) const { return terms[idx+1] - terms[idx]; }
    inline int size() const { return bucket_offsets[th.buckets()]; }
    inline int buckets() const { return th.buckets(); }
    inline int bucket_size(int x) const { return bucket_offsets[x+1] - bucket_offsets[x]; }
    inline TermHasher hasher() const { return th; }
};

#endif

