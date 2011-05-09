#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "bufrw.h"
#include "fileio.h"
#include "term.h"

const int max_bucket_size = 13;
struct term {
    int offset, length;
};
static TermHasher th;
static term *hashtable;
static char *texts;
static int texts_used, texts_alloc;
static int total_term_count;

static void insert_word(const char *start, const char *end)
{
    size_t length = end-start;
    int hash = th(start, length);

    if(length >= 128) {
        fprintf(stderr, "WARNING: term too long (%d >= 128): '%.*s', skipping\n", (int)length, (int)length, start);
        return;
    }

    int pos = hash*max_bucket_size;
    while(pos < (hash+1)*max_bucket_size)
        if(hashtable[pos].length == 0)
            break;
        else if(hashtable[pos].length == length &&
                memcmp(texts+hashtable[pos].offset, start, length) == 0)
            return;
        else
            pos++;
    assert(hashtable[pos].length == 0);

    hashtable[pos].offset = texts_used;
    hashtable[pos].length = length;

    if(texts_used + length >= texts_alloc) {
        while(texts_used + length >= texts_alloc)
            texts_alloc *= 2;
        texts = (char *)realloc(texts, texts_alloc);
        printf("reallocating texts to %d megabytes\n", texts_alloc>>20);
        assert(texts);
    }

    memcpy(texts + texts_used, start, length);
    texts_used += length;
    total_term_count++;
}

static void add_words(const char *start, const char *end)
{
    while(start < end)
    {
        while(start < end && (*start == ' ' || *start == '\n')) start++;
        const char *p = start;
        while(start < end && *start != ' ' && *start != '\n') start++;
        if(start - p > 0) 
            insert_word(p,  start);
    }
}

int main(int argc, char *argv[])
{
    th.a = 123456337;
    th.b = 203451187;
    th.n = 3984301;

    hashtable = (term *)calloc(th.n * max_bucket_size, sizeof(term));
    texts_alloc = 8*1048576; texts_used = 0;
    texts = (char *)malloc(texts_alloc);

    for(int i=1; i<argc; i++) {
        printf("reading file %d\n", i);
        FileMapping map(argv[i]);
        add_words((const char *)map.data(),
                  (const char *)map.end());
    }
   
    Writer wr;

    printf("writing header\n");
    wr.write_u32(0x50524f43);
    wr.write_u32(th.a);
    wr.write_u32(th.b);
    wr.write_u32(th.n);

    printf("writing bucket sizes\n");
    int max_occ = 0;
    for(int i=0; i<th.n; i++) {
        int size = 0;
        while(size < max_bucket_size && hashtable[i*max_bucket_size + size].length)
            size++;
        if(size > max_occ) max_occ = size;
        wr.write_u8(size);
    }
    printf("max occupancy: %d, term count: %d\n", max_occ, total_term_count);

    printf("writing word lengths\n");
    for(int i=0; i<th.n*max_bucket_size; i++)
        if(hashtable[i].length) 
            wr.write_u8(hashtable[i].length);

    printf("writing result\n");
    for(int i=0; i<th.n*max_bucket_size; i++)
        if(hashtable[i].length) 
            wr.write_raw(texts+hashtable[i].offset, hashtable[i].length);

    printf("flushing\n");
    FileIO fio("db/corpus", O_WRONLY|O_CREAT|O_TRUNC);
    fio.write_raw(wr.buffer(), wr.tell());

    return 0;
}


