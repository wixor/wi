#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "corpus.h"
#include "rawpost.h"

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
            if(term_id == -1) 
                fprintf(stderr, "WARNING: term not found: '%.*s'\n", (int)(p-start), start);
            else {
                uint64_t x = rawpost(term_id, doc_id, term_pos++);
                fwrite_unlocked(&x, sizeof(x), 1, stdout);
            }
        }
    }
}

int main(int argc, char *argv[])
{
    Corpus corp("db/corpus");
    int total_terms = 0;

    setvbuf(stdin,  (char *)malloc(1048576*16), _IOFBF, 1048576*16);
    setvbuf(stdout, (char *)malloc(1048576*16), _IOFBF, 1048576*16);

    for(;;)
    {
        static char buf[16*1048576];
        const char *end;
       
        if(doc_id % 1000 == 0) 
            fprintf(stderr, "processed articles: %d, total terms: %d\r", doc_id, total_terms);
        if(fgets(buf, sizeof(buf)-1, stdin) != buf)
            break;

        end = strchr(buf, '\n');
        run_line(buf, end, corp);

        total_terms += term_pos;
        doc_id++;
        term_pos = 0;
    }
    fprintf(stderr, "\n");
    return 0;
}

