extern "C" {
#include <talloc.h>
}

#include "bufrw.h"
#include "fileio.h"
#include "term.h"

static const char **terms;
static int *bucket_offs;
static TermHasher th;

static void read_dict(const char *filename)
{
    void *memctx = talloc_named_const(NULL, 0, "dictionary");
    FileMapping fmap(filename);
    Reader rd(fmap.data(), fmap.size());

    /* check magic value */
    assert(rd.read_u32() == 0x54434944);

    /* read hash function parameters */
    th.a = rd.read_u32();
    th.b = rd.read_u32();
    th.n = rd.read_u32();

    /* read lemmatized lists count */
    int bucket_count = th.buckets(),
        lemmatized_list_count = rd.read_u32();

    /* read bucket sizes */
    bucket_offs = talloc_array(memctx, int, bucket_count+1);
    assert(bucket_offs);

    bucket_offs[0] = 0;
    for(int i=1; i<=bucket_count; i++)
        bucket_offs[i] = bucket_offs[i-1] + rd.read_u8();

    /* read term lengths */
    int term_count = bucket_offs[bucket_count];
    terms = talloc_array(memctx, const char *, term_count+1);
    assert(terms);

    terms[0] = (const char *)0L;
    for(int i=1; i<=term_count; i++) {
        rd.read_u24();
        terms[i] = terms[i-1] + rd.read_u8();
    }

    /* read posting list info */
    for(int i=0; i<term_count; i++) 
        rd.read_uv(), rd.read_uv();
    for(int i=0; i<lemmatized_list_count; i++) 
        rd.read_uv(), rd.read_uv();

    /* read term texts */
    int term_texts_length = (long)terms[term_count];
    char *term_texts = talloc_array(memctx, char, term_texts_length);
    assert(term_texts);
    rd.read_raw(term_texts, term_texts_length);

    for(int i=0; i<=term_count; i++)
        terms[i] = term_texts + (long)terms[i];

    /* make sure we read everything */
    assert(rd.eof());
}

static int lookup(const char *text, size_t len)
{
    int h = th(text, len);
    
    const char **t = terms + bucket_offs[h],
               **tend = terms + bucket_offs[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)(t[1] - t[0]))
            continue;
        if(memcmp(text, *t, len) != 0)
            continue;
        return bucket_offs[h] + i;
    }
    return -1;
}

int main(int argc, char *argv[])
{
    read_dict("db/dictionary");

    Writer wr;
    wr.write_u32(0x504f5453);
    wr.write_u32(0);

    static char buf[1024];
    int count = 0;
    while(fgets(buf, sizeof(buf)-1, stdin) == buf)
    {
        int len = strlen(buf);
        buf[len-1] = '\0'; len--;

        int key = lookup(buf, len);
        if(key == -1) {
            fprintf(stderr,"word not found: »%s«\n", buf);
            return 1;
        }
        wr.write_u32(key);
        count++;
    }

    ((uint32_t *)wr.buffer())[1] = count;

    FileIO stopwords("db/stopwords", O_WRONLY|O_CREAT|O_TRUNC);
    stopwords.write_raw(wr.buffer(), wr.tell());

    return 0;
}

