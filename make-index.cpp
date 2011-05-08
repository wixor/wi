extern "C" {
#include <talloc.h>
}
#include <assert.h>
#include "bufrw.h"
#include "fileio.h"
#include "corpus.h"

struct term {
    int id;
    int length;
    int bucket;
    int alias;
    struct {
        int id, n_entries, length;
    } lemmatized;
    struct {
        int n_entries, length;
    } positional;
    bool empty;
};

static void seek_until(int id, Reader *rd)
{
    while(!rd->eof())
    {
        size_t save = rd->tell();
        uint64_t e = rd->read_u64();

        int term_id = e >> 39;
        if(term_id < id)
            continue;
        rd->seek(save);
        break;
    }
}

/* term id: 25 bits -- up to 32 mln terms (real: ??)
 * document id: 20 bits -- up to 1 mln documents (real: 800 k)
 * offset in document: 19 bits -- up to .5 mln terms/document (real max: 50 k)
 */
static bool write_lemmatized(struct term *term, Reader *rd, const FileIO &fio)
{
    seek_until(term->id, rd);

    Writer wr;

    int last_doc_id = -1, n_entries = 0;
    while(!rd->eof())
    {
        size_t save = rd->tell();
        uint64_t e = rd->read_u64();

        int term_id = e >> 39;
        if(term_id > term->id) {
            rd->seek(save);
            break;
        }

        int doc_id = (e>>19)  & 0xfffff;
        if(doc_id == last_doc_id)
            continue;
        if(last_doc_id == -1) 
            wr.write_u24(doc_id);
        else
            wr.write_uv(doc_id - last_doc_id);
        last_doc_id = doc_id;
        n_entries++;
    }

    term->lemmatized.n_entries = n_entries;
    term->lemmatized.length = wr.tell();
    if(n_entries) {
        fio.write_raw(wr.buffer(), wr.tell());
        talloc_free(wr.buffer());
        return true;
    } else {
        talloc_free(wr.buffer());
        return false;
    }
}

static bool write_positional(struct term *term, Reader *rd, const FileIO &fio)
{
    seek_until(term->id, rd);

    Writer docswr, poswr;
    docswr.write_u32(0);

    int last_doc_id = -1, last_pos = 0, pos_start = 0, n_entries = 0;
    while(!rd->eof())
    {
        size_t save = rd->tell();
        uint64_t e = rd->read_u64();

        int term_id = e >> 39;
        if(term_id > term->id) {
            rd->seek(save);
            break;
        }

        int doc_id = (e>>19)  & 0xfffff;
        int pos = e & 0x7ffff;

        if(doc_id == last_doc_id) {
            poswr.write_uv(pos - last_pos);
            last_pos = pos;
            continue;
        }

        if(last_doc_id == -1) { 
            docswr.write_u24(doc_id);
            pos_start = poswr.tell();
            poswr.write_uv(pos);
            last_pos = pos;
        }
        else {
            docswr.write_uv(poswr.tell() - pos_start);
            docswr.write_uv(doc_id - last_doc_id);
            pos_start = poswr.tell();
            poswr.write_uv(pos);
            last_pos = pos;
        }

        last_doc_id = doc_id;
        n_entries++;
    }
    if(last_doc_id != -1) 
        docswr.write_uv(poswr.tell() - pos_start);
    *(uint32_t *)docswr.buffer() = docswr.tell() - 4;

    if(!n_entries) {
        term->positional.n_entries = 0;
        term->positional.length = 0;
        talloc_free(docswr.buffer());
        talloc_free(poswr.buffer());
        return false;
    }

    term->positional.n_entries = n_entries;
    term->positional.length = docswr.tell() + poswr.tell();

    fio.write_raw(docswr.buffer(), docswr.tell());
    fio.write_raw(poswr.buffer(), poswr.tell());

    talloc_free(docswr.buffer());
    talloc_free(poswr.buffer());
    return true;
}

int main(void)
{
    FileMapping invmap("db/inverted"),
                invlemmap("db/invlemma"), 
                aliasmap("db/aliases");

    Reader inv(invmap.data(), invmap.size()),
           invlem(invlemmap.data(), invlemmap.size()),
           alird(aliasmap.data(), aliasmap.size());

    FileIO positional("db/positional", O_WRONLY|O_CREAT|O_TRUNC),
           lemmatized("db/lemmatized", O_WRONLY|O_CREAT|O_TRUNC);

    positional.write_raw("IDXP", 4);
    lemmatized.write_raw("IDXL", 4);

    Corpus corp("db/corpus");

    assert(alird.read_u32() == 0x41494c41);
    assert(alird.read_u32() == corp.size());
    int *aliases = (int *)aliasmap.data() + 2;
    int n_terms = corp.size();

    term *terms = (term *)malloc(sizeof(struct term) * n_terms);

    int lemmatized_list_count = 0;
    for(int i=0, buck=0, bidx=0; i<n_terms; i++)
    {
        term *term = terms+i;

        term->id = i;
        term->alias = aliases[i];
        assert(term->alias <= i);

        term->length = corp.term_length(i);
        term->bucket = buck;
        assert(buck < corp.buckets());

        bidx++;
        while(buck < corp.buckets() && bidx >= corp.bucket_size(buck)) {
            buck++;
            bidx = 0;
        }

        bool has_lemmatized = write_lemmatized(term, &invlem, lemmatized),
             has_positional = write_positional(term, &inv, positional);

        assert(term->alias == i || !has_lemmatized);
        
        term->empty = true;
        term->empty = !has_lemmatized && !has_positional && terms[term->alias].empty;
        
        if(term->alias != i)
            term->lemmatized.id = terms[term->alias].lemmatized.id;
        else if(!term->empty)
            term->lemmatized.id = lemmatized_list_count++;
    }

    int n_buckets = corp.buckets();
    int *bucket_sizes = (int *)malloc(sizeof(int) * n_buckets);
    memset(bucket_sizes, 0, sizeof(int) * n_buckets);
    for(int i=0; i<n_terms; i++)
        if(!terms[i].empty)
            bucket_sizes[terms[i].bucket]++;
    
    Writer wr;
    wr.write_u32(0x54434944);
    wr.write_u32(corp.hasher().a);
    wr.write_u32(corp.hasher().b);
    wr.write_u32(corp.hasher().n);
    wr.write_u32(lemmatized_list_count);

    for(int i=0; i<n_buckets; i++)
        wr.write_u8(bucket_sizes[i]);

    for(int i=0; i<n_terms; i++)
    {
        const term *term = terms+i;
        if(term->empty)
            continue;
        wr.write_u24(term->lemmatized.id);
        wr.write_u8(term->length);
    }
    
    for(int i=0; i<n_terms; i++)
    {
        const term *term = terms+i;
        if(term->empty)
            continue;
        wr.write_uv(term->positional.length);
        wr.write_uv(term->positional.n_entries);
    }
    
    for(int i=0, j=0; i<lemmatized_list_count; i++)
    {
        while(j < n_terms && (terms[j].empty || terms[j].lemmatized.id != i)) j++;
        assert(j < n_terms);
        wr.write_uv(terms[j].lemmatized.length);
        wr.write_uv(terms[j].lemmatized.n_entries);
    }

    for(int i=0; i<n_terms; i++)
    {
        const term *term = terms+i;
        if(term->empty)
            continue;
        char buf[term->length+1];
        corp.lookup(i, buf, term->length+1);
        wr.write_raw(buf, term->length);
    }
    
    FileIO dictionary("db/dictionary", O_WRONLY|O_CREAT|O_TRUNC);
    dictionary.write_raw(wr.buffer(), wr.tell());

    return 0;
}
