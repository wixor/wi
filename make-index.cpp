#include <math.h>
#include <assert.h>
#include <string.h>
extern "C" {
#include <talloc.h>
}
#include "bufrw.h"
#include "fileio.h"
#include "corpus.h"
#include "rawpost.h"

template<typename T>
static inline T square(const T &x) { return x*x; }

class IndexMaker
{
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
        bool has_lemmatized, has_positional, empty;
    };
    

    void *memctx;

    int n_terms;
    struct term *terms;
    int dummy_term_id, marker_term_id;

    TermHasher th;
    int *bucket_sizes;
    
    int lemmatized_list_count;

    int n_articles;
    uint16_t *article_lengths;
    float *tfidf_weights;


    static void seek_until(int term_id, Reader *rd);
    class PositionalWriter
    {
        IndexMaker *im;
        FileMapping fmap;
        Reader rd;
        Writer docs, positions;
        FileIO fio;

    public:
        inline PositionalWriter(IndexMaker *im);
        inline ~PositionalWriter();
        inline bool run(struct term *term);
    };
    class LemmatizedWriter
    {
        IndexMaker *im;
        FileMapping fmap;
        Reader rd;
        Writer wr;
        FileIO fio;
    public:
        inline LemmatizedWriter(IndexMaker *im);
        inline ~LemmatizedWriter();
        inline bool run(struct term *term, bool rank);
    };

    inline void read_artitles();
    inline void read_corpus();
    inline void read_aliases();
    inline void process_positional();
    inline void process_lemmatized();
    inline void resolve_aliases();
    inline void compute_bucket_sizes();
    inline void write_dict();
    inline void write_tfidf_weights();

    void do_run();
public:
    static inline void run() {
        IndexMaker().do_run();
    }
};

void IndexMaker::seek_until(int id, Reader *rd)
{
    while(!rd->eof())
    {
        size_t save = rd->tell();
        rawpost e(rd->read_u64());

        if(e.term_id() < id)
            continue;
        rd->seek(save);
        break;
    }
}

IndexMaker::PositionalWriter::PositionalWriter(IndexMaker *im) {
    this->im = im;
    fmap.attach("db/inverted");
    rd.attach(fmap.data(), fmap.size());
    fio.open("db/positional", O_WRONLY|O_CREAT|O_TRUNC);
    fio.write_raw("IDXP", 4);
}
IndexMaker::PositionalWriter::~PositionalWriter() {
    fio.close();
    docs.free();
    positions.free();
}
bool IndexMaker::PositionalWriter::run(struct term *term)
{
    /* seek until postings for requested term */
    seek_until(term->id, &rd);

    /* initiate docs and positions buffers.
     * put placeholder for docs list length */
    positions.rewind();
    docs.rewind();
    docs.write_u32(0);

    int last_doc_id = -1, /* last document id seen */
        last_pos = 0, /* last term position seen */
        pos_start = 0, /* where term positions for current document start */
        n_entries = 0; /* how many different documents there are */
    
    while(!rd.eof()) /* for each posting for the term */
    {
        size_t save = rd.tell();
        rawpost e(rd.read_u64());

        if(e.term_id() > term->id) {
            rd.seek(save);
            break;
        }

        if(e.doc_id() == last_doc_id) {
            /* next posting for last document, write down the position */
            positions.write_uv(e.term_pos() - last_pos);
            last_pos = e.term_pos();
        } else {
            /* first posting for next document */
            if(last_doc_id == -1) {
                /* first document, write down doc_id */
                docs.write_u24(e.doc_id());
            } else {
                /* next docment, write down length of last doc's position
                 * list and new doc_id */
                docs.write_uv(positions.tell() - pos_start);
                docs.write_uv(e.doc_id() - last_doc_id);
            }
            /* either way, record where positions for this doc start
             * and write down the position */
            pos_start = positions.tell();
            positions.write_uv(e.term_pos());
            last_pos = e.term_pos();

            /* remember we have new document */
            last_doc_id = e.doc_id();
            n_entries++;
        }
    }
    
    /* write down last document's position list length */
    if(last_doc_id != -1) 
        docs.write_uv(positions.tell() - pos_start);

    /* fill in length of documents list */
    *(uint32_t *)docs.buffer() = docs.tell() - 4;

    /* see if we got anything, save to file if so */
    if(!n_entries) {
        term->positional.n_entries = 0;
        term->positional.length = 0;
        return false;
    } else {
        term->positional.n_entries = n_entries;
        term->positional.length = docs.tell() + positions.tell();
        fio.write_raw(docs.buffer(), docs.tell());
        fio.write_raw(positions.buffer(), positions.tell());
        return true;
    }
}

IndexMaker::LemmatizedWriter::LemmatizedWriter(IndexMaker *im) {
    this->im = im;
    fmap.attach("db/invlemma");
    rd.attach(fmap.data(), fmap.size());
    fio.open("db/lemmatized", O_WRONLY|O_CREAT|O_TRUNC);
    fio.write_raw("IDXL", 4);
}
IndexMaker::LemmatizedWriter::~LemmatizedWriter() {
    fio.close();
    wr.free();
}
bool IndexMaker::LemmatizedWriter::run(struct term *term, bool rank)
{
    /* seek until postings for requested term */
    seek_until(term->id, &rd);

    int n_entries = 0; /* how many different documents there are */
    {
        size_t save = rd.tell();
        int last_doc_id = -1; /* last document id seen */
        while(!rd.eof()) { 
            rawpost e(rd.read_u64());
            if(e.term_id() > term->id) 
                break;
            if(e.doc_id() != last_doc_id) {
                n_entries++;
                last_doc_id = e.doc_id();
            }
        }
        rd.seek(save);
    }

    /* if no documents have the term, quit now */
    if(!n_entries)
        return false;

    /* this tells us, how frequent the term is among documents */
    assert(n_entries <= im->n_articles);
    float idf = logf((float)im->n_articles / (float)n_entries);
    assert(idf >= 0.f);

    /* init postings buffer */
    wr.rewind();

    int last_doc_id = -1, /* last document id seen */
        n_pos = 0; /* how many times the term appears in the document (tf) */
    
    while(!rd.eof()) /* for each posting for the term */
    {
        size_t save = rd.tell();
        rawpost e(rd.read_u64());

        if(e.term_id() > term->id) {
            rd.seek(save);
            break;
        }

        if(e.doc_id() == last_doc_id) {
            /* next posting for last document, count it */ 
            n_pos++;
        } else {
            /* first posting for next document */
            if(last_doc_id == -1)  {
                /* first document ever, write down doc_id */
                wr.write_u24(e.doc_id());
            } else {
                /* next document, write down how many times the term appeared
                 * in last document. also update its tfidf weights, since we
                 * now have all info at hand. then move on to next document. */
                if(likely(rank)) {
                    assert(n_pos && n_pos <= im->article_lengths[last_doc_id]);
                    float tf = (float)n_pos / (float)im->article_lengths[last_doc_id];
                    im->tfidf_weights[last_doc_id] += square(tf * idf);
                }
                wr.write_uv(n_pos);
                wr.write_uv(e.doc_id() - last_doc_id);
            }
            /* either way, we have a new document */        
            last_doc_id = e.doc_id();
            n_pos = 1;
        }
    }

    /* write down tf for last document and update its tfidf weight */
    if(last_doc_id != -1) {
        if(likely(rank)) {
            assert(n_pos && n_pos <= im->article_lengths[last_doc_id]);
            float tf = (float)n_pos / (float)im->article_lengths[last_doc_id];
            im->tfidf_weights[last_doc_id] += square(tf * idf);
        }
        wr.write_uv(n_pos);
    }

    /* save what we've got to file */
    term->lemmatized.n_entries = n_entries;
    term->lemmatized.length = wr.tell();
    fio.write_raw(wr.buffer(), wr.tell());
    return true;
}

void IndexMaker::do_run()
{
    memctx = talloc_named_const(NULL, 0, "index maker");
    assert(memctx);

    read_artitles();
    read_corpus();
    read_aliases();
    process_positional();
    process_lemmatized();
    resolve_aliases();
    compute_bucket_sizes();
    write_dict();
    write_tfidf_weights();

    talloc_free(memctx);
}

void IndexMaker::read_artitles()
{
    printf("reading artitles...\n");

    uint32_t hdr[2];
    FileIO fio("db/artitles", O_RDONLY);
    fio.read_raw(hdr, sizeof(hdr));

    assert(hdr[0] == 0x4c544954);
    n_articles = hdr[1];

    tfidf_weights = talloc_array(memctx, float, n_articles);
    article_lengths = talloc_array(memctx, uint16_t, n_articles);
    assert(tfidf_weights && article_lengths);

    memset(tfidf_weights, 0, sizeof(float)*n_articles);
    fio.read_raw(article_lengths, sizeof(uint16_t)*n_articles, 8+8*n_articles);

    fio.close();
}

void IndexMaker::read_corpus()
{
    printf("reading corpus...\n");

    Corpus corp("db/corpus");
    n_terms = corp.size();
    th = corp.hasher();

    dummy_term_id = corp.lookup("\"", 1);
    marker_term_id = corp.lookup("\"m", 2);

    terms = talloc_array(memctx, struct term, n_terms);
    assert(terms);
    
    for(int i=0, buck=0, bidx=0; i<n_terms; i++)
    {
        term *term = terms+i;

        term->id = i;
        term->length = corp.term_length(i);
        term->bucket = buck;
        assert(buck < th.buckets());

        bidx++;
        while(buck < th.buckets() && bidx >= corp.bucket_size(buck)) {
            buck++;
            bidx = 0;
        }
    }
}

void IndexMaker::read_aliases()
{
    printf("reading aliases...\n");

    FileMapping fmap("db/aliases");
    Reader rd(fmap.data(), fmap.size());

    rd.assert_u32(0x41494c41);
    rd.assert_u32(n_terms);

    for(int i=0; i<n_terms; i++) {
        terms[i].alias = rd.read_u32();
        assert(terms[i].alias <= i);
    }
}

void IndexMaker::process_positional()
{
    printf("processing positional lists...\n");

    PositionalWriter poswr(this);
    for(int i=0; i<n_terms; i++)
        if(i != dummy_term_id && i != marker_term_id)
            terms[i].has_positional = poswr.run(terms+i);
}

void IndexMaker::process_lemmatized()
{
    printf("processing lemmatized lists...\n");

    LemmatizedWriter lemwr(this);
    for(int i=0; i<n_terms; i++)
        if(i != dummy_term_id)
            terms[i].has_lemmatized = lemwr.run(terms+i, i != marker_term_id);
}

void IndexMaker::resolve_aliases()
{
    printf("resolving aliases...\n");

    lemmatized_list_count = 0;
    for(int i=0; i<n_terms; i++)
    {
        term *term = terms+i;

        assert(term->alias == i || !term->has_lemmatized);
        
        term->empty = true;
        term->empty = !term->has_lemmatized &&
                      !term->has_positional &&
                      terms[term->alias].empty;
        
        if(term->alias != i)
            term->lemmatized.id = terms[term->alias].lemmatized.id;
        else if(!term->empty)
            term->lemmatized.id = lemmatized_list_count++;
    }
}

void IndexMaker::compute_bucket_sizes()
{
    printf("computing bucket sizes...\n");

    bucket_sizes = talloc_array(memctx, int, th.buckets());
    assert(bucket_sizes);

    memset(bucket_sizes, 0, sizeof(int) * th.buckets());
    for(int i=0; i<n_terms; i++)
        if(!terms[i].empty)
            bucket_sizes[terms[i].bucket]++;
}

void IndexMaker::write_dict()
{
    printf("writing dict...\n");

    Writer wr;
    wr.write_u32(0x54434944);
    wr.write_u32(th.a);
    wr.write_u32(th.b);
    wr.write_u32(th.n);
    wr.write_u32(lemmatized_list_count);

    for(int i=0; i<th.buckets(); i++)
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

    Corpus corp("db/corpus");
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
}

void IndexMaker::write_tfidf_weights()
{
    printf("writing tfidf weights...\n");

    /* compute actual inverses of vector lengths and save them */
    for(int i=0; i<n_articles; i++)
        tfidf_weights[i] =
            tfidf_weights[i] != 0.f ? 1.f/sqrtf(tfidf_weights[i]) : 0.f;

    FileIO fio("db/artitles", O_WRONLY);
    fio.write_raw(tfidf_weights, sizeof(float)*n_articles, 8);
    fio.close();
}

int main(void) {
    IndexMaker::run();
    return 0;
}

