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
    inline int size() const { return bucket_offsets[th.buckets()]; }
};

#endif

