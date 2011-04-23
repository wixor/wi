#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "corpus.h"

/* term id: 25 bits -- up to 32 mln terms (real: ??)
 * document id: 20 bits -- up to 1 mln documents (real: 800 k)
 * offset in document: 19 bits -- up to .5 mln terms/document (real max: 50 k)
 */

static uint64_t compose(int term_id, int doc_id, int term_pos)
{
    assert(term_id >= 0 && term_id < (1<<25));
    assert(doc_id >= 0 && doc_id < (1<<20));
    assert(term_pos >= 0 && term_pos < (1<<19));

    return ((uint64_t)term_id << 39) |
           ((uint64_t)doc_id << 19) |
           ((uint64_t)term_pos << 0);
}

static int doc_id, term_pos;

static void run_line(const char *p, const char *end, const Corpus &corp)
{
    while(p < end)
    {
        while(p < end && *p == ' ') p++;
        const char *start = p;
        while(p < end && *p != ' ') p++;

        if(p - start > 0)
        {
            int term_id = corp.lookup(start, p-start);
            assert(term_id != -1);

            uint64_t x = compose(term_id, doc_id, term_pos++);
            fwrite_unlocked(&x, sizeof(x), 1, stdout);
        }
    }
}

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "digitize [corpus file] < [text file] > [output]\n");
        return 1;
    }

    Corpus corp(argv[1]);
    int total_terms = 0;

    for(;;)
    {
        static char buf[1048576];
        const char *end;
        
        if(fgets(buf, sizeof(buf)-1, stdin) == NULL) break;
        end = strchr(buf, '\n');
        run_line(buf, end, corp);
        
        assert(fgets(buf, sizeof(buf)-1, stdin) == buf);
        end = strchr(buf, '\n');
        run_line(buf, end, corp);

        total_terms += term_pos;
        doc_id++;
        term_pos = 0;
        fprintf(stderr, "processed articles: %d, total terms: %d\r", doc_id, total_terms);
    }
    fprintf(stderr, "\n");
    return 0;
}

