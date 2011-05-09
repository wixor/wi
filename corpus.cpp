#include <assert.h>
extern "C" {
	#include <talloc.h>
}
#include "bufrw.h"
#include "fileio.h"
#include "corpus.h"

Corpus::Corpus() {
    memctx = NULL;
}
Corpus::Corpus(const char *filename) {
    memctx = NULL;
    read(filename);
}
Corpus::~Corpus() {
    if(memctx)
        talloc_free(memctx);
}
void Corpus::read(const char *filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "corpus");
    assert(memctx);

    FileMapping fmap(filename);
    Reader rd(fmap.data(), fmap.size());

    assert(rd.read_u32() == 0x50524f43);
    th.a = rd.read_u32();
    th.b = rd.read_u32();
    th.n = rd.read_u32();

    bucket_offsets = talloc_array(memctx, int, th.buckets()+1);
    assert(bucket_offsets);

    bucket_offsets[0] = 0;
    for(int i=1; i<=th.buckets(); i++)
        bucket_offsets[i] = bucket_offsets[i-1] + rd.read_u8();

    int term_count = bucket_offsets[th.buckets()];
    terms = talloc_array(memctx, const char *, term_count+1);
    assert(terms);

    terms[0] = NULL;
    for(int i=1; i<=term_count; i++)
        terms[i] = terms[i-1] + rd.read_u8();

    char *all_terms = (char *)talloc_size(memctx, (long)terms[term_count]);
    assert(all_terms);

    rd.read_raw(all_terms, (long)terms[term_count]);
    for(int i=0; i<=term_count; i++)
        terms[i] = all_terms + (long)terms[i];

    assert(rd.eof());
}

int Corpus::lookup(const char *text, size_t len) const
{
    int hash = th(text, len);
    for(int idx = bucket_offsets[hash]; idx < bucket_offsets[hash+1]; idx++)
    {
        if(len != terms[idx+1] - terms[idx])
            continue;
        if(memcmp(text, terms[idx], len) == 0)
            return idx;
    }
    return -1;
}

void Corpus::lookup(int idx, char *out, size_t len) const
{
    assert(0 <= idx && idx < bucket_offsets[th.buckets()]);
    size_t n = terms[idx+1] - terms[idx];
    assert(n < len);
    memcpy(out, terms[idx], n);
    out[n] = '\0';
}
