#include <string.h>
#include <talloc.h>
#include "bufrw.h"
#include "term.h"
#include "fileio.h"

class Dictionary
{
public:
    struct PostingList {
        off_t offset;
        int n_postings;
    };

    struct Term {
        int lemmatized_list_id;
        short text_length;
        bool stop;
        char *text;
    };

private:
    void *memctx;

    TermHasher hasher;
    PostingList *lemmatized, *positional;
    Term *terms, **buckets;

    void do_read(Reader rd);

public:
    struct Info {
        struct {
            int id, length, offset, n_postings;
        } lemmatized, positional;
        int bucket;
        bool stop;
    };

    Dictionary();
    ~Dictionary();

    void read(const char *filename);
    Info lookup(const char *text, size_t len) const;
};

Dictionary::Dictionary() {
    memctx = NULL;
}
Dictionary::~Dictionary() {
    if(memctx) talloc_free(memctx);
}

void Dictionary::read(const char *filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "dictionary");
    assert(memctx);

    FileMapping fmap(filename);
    do_read(Reader(fmap.data(), fmap.size()));
}

void Dictionary::do_read(Reader rd)
{
    /* check magic value */
    if(rd.read_u32() != 0x54434944) abort();

    /* read hash function parameters */
    hasher.a = rd.read_u32();
    hasher.b = rd.read_u32();
    hasher.n = rd.read_u32();

    /* read lemmatized lists count */
    int bucket_count = hasher.buckets(),
        lemmatized_list_count = rd.read_u32();

    /* save bucket size reader for future use */
    Reader bucket_size_rd = rd;

    /* count terms */
    int term_count = 0;
    for(int i=0; i<bucket_count; i++)
        term_count += rd.read_u8();

    /* allocate stuff */
    lemmatized = talloc_array(memctx, PostingList, lemmatized_list_count+1);
    positional = talloc_array(memctx, PostingList, term_count+1);
    buckets = talloc_array(memctx, Term*, bucket_count+1);
    terms = talloc_array(memctx, Term, term_count);
    assert(lemmatized && positional && terms && buckets);

    { /* read bucket info */
        Term *t = terms;
        for(int i=0; i<bucket_count; i++)
        {
            buckets[i] = t;
            int bucket_size = bucket_size_rd.read_u8();
            while(bucket_size--) {
                t->lemmatized_list_id = rd.read_u24();
                t->text_length = rd.read_u8();
                if(t->text_length & 0x80) {
                    t->stop = true;
                    t->text_length &=~ 0x80;
                }
                t++;
            }
        }
        buckets[bucket_count] = t;
    }

    /* read positional list info */
    positional[0].offset = 4;/* because of index file magic number */
    for(int i=1; i<=term_count; i++) {
        positional[i].offset = positional[i-1].offset + rd.read_uv();;
        positional[i-1].n_postings = rd.read_uv();
    }

    /* read lemmatized list info */
    lemmatized[0].offset = 4; /* because of index file magic number */
    for(int i=1; i<=lemmatized_list_count; i++) {
        lemmatized[i].offset = lemmatized[i-1].offset + rd.read_uv();;
        lemmatized[i-1].n_postings = rd.read_uv();
    }

    /* compute total texts length */
    int term_texts_length = 0;
    for(int i=0; i<term_count; i++)
        term_texts_length += terms[i].text_length;

    /* read term texts */
    char *term_texts = (char *)talloc_size(memctx, term_texts_length);
    assert(term_texts);
    rd.read_raw(term_texts, term_texts_length);

    /* bind term texts to terms */
    terms[0].text = term_texts;
    for(int i=1; i<term_count; i++)
        terms[i].text = terms[i-1].text + terms[i-1].text_length;

    printf("dictionary: a=%d, b=%d, n=%d, bucket_count=%d\n"
           "lemmatized_list_count=%d, term_count=%d, term_texts_length=%d\n"
           "tail=%d\n",
            hasher.a,hasher.b,hasher.n,bucket_count,lemmatized_list_count,term_count,term_texts_length,(int)(rd.size()-rd.tell()));

    /* make sure we read everything */
    assert(rd.eof());
}

Dictionary::Info Dictionary::lookup(const char *text, size_t len) const
{
    int h = hasher.hash(text, len);

    const Term *t = buckets[h], *tend = buckets[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)t->text_length)
            continue;
        if(memcmp(text, t->text, len) != 0)
            continue;
    
        int idx = buckets[h] - buckets[0] + i;
        Dictionary::Info info;
        info.positional.id = idx;
        info.positional.length = positional[idx+1].offset - positional[idx].offset;
        info.positional.offset = positional[idx].offset;
        info.positional.n_postings = positional[idx].n_postings;
        info.lemmatized.id = terms[idx].lemmatized_list_id;
        info.lemmatized.length = lemmatized[info.lemmatized.id+1].offset - lemmatized[info.lemmatized.id].offset;
        info.lemmatized.offset = lemmatized[info.lemmatized.id].offset;
        info.lemmatized.n_postings = lemmatized[info.lemmatized.id].n_postings;
        info.bucket = h;
        info.stop = terms[idx].stop;
        return info;
    }

    Dictionary::Info info;
    info.positional.id = -1;
    return info;
}

int main(int argc, char *argv[])
{
    Dictionary dict; dict.read("db/dictionary");
    FileMapping lemmatized("db/lemmatized"),
                positional("db/positional");

    for(int i=1; i<argc; i++)
    {
        Dictionary::Info info = dict.lookup(argv[i], strlen(argv[i]));
        if(info.positional.id == -1) {
            printf("word »%s«: not found\n", argv[i]);
            continue;
        }

        printf("word »%s«: id %d, bucket %d%s\n", argv[i], info.positional.id, info.bucket, info.stop ? ", stopword" : "");

        printf("  lemmatized list: id %d, length %d, offset %d, postings %d\n",
                info.lemmatized.id, info.lemmatized.length, info.lemmatized.offset, info.lemmatized.n_postings);
        if(info.lemmatized.length) {
            Reader rd((const char *)lemmatized.data() + info.lemmatized.offset, info.lemmatized.length);

            int doc_id = rd.read_u24();
            printf("    %d\n", doc_id);
            for(int i=1; i<info.lemmatized.n_postings; i++) {
                doc_id += rd.read_uv();
                printf("    %d\n", doc_id);
            }
        }

        printf("  positional list: length %d, offset %d, postings %d\n",
                info.positional.length, info.positional.offset, info.positional.n_postings);
        if(info.positional.length) {
            Reader rd((const char *)positional.data() + info.positional.offset, info.positional.length);

            int ddl = rd.read_u32();
            printf("    docs description length: %d bytes\n", ddl);

            int doc_id = rd.read_u24(), pl = rd.read_uv();
            for(int i=0; i<info.positional.n_postings; i++)
            {
                printf("    %d (positions: %d bytes):", doc_id, pl);
                Reader prd((const char *)rd.buffer() + 4+ddl, pl);
                int p = 0;
                while(!prd.eof()) {
                    p += prd.read_uv();
                    printf(" %d", p);
                }
                ddl += pl;
                if(i != info.positional.length-1) {
                    doc_id += rd.read_uv();
                    pl = rd.read_uv();
                }
                printf("\n");
            }
        }
    }

}

