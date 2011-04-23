#include <stdio.h>
#include "fileio.h"
#include "corpus.h"

/* term id: 25 bits -- up to 32 mln terms (real: ??)
 * document id: 20 bits -- up to 1 mln documents (real: 800 k)
 * offset in document: 19 bits -- up to .5 mln terms/document (real max: 50 k)
 */

int main(int argc, char *argv[])
{
    if(argc != 3) {
        fprintf(stderr, "dedigitize [corpus] [digitize output]\n");
        return 1;
    }

    Corpus corp(argv[1]);
    FileMapping fmap(argv[2]);
    const uint64_t *p   = (const uint64_t *)fmap.data(),
                   *end = (const uint64_t *)fmap.end();
    while(p < end)
    {
        uint64_t x = *p++;
        int term_off = (x >> 0)  & ((1<<19) - 1),
            doc_id   = (x >> 19) & ((1<<20) - 1),
            term_id  = (x >> 39) & ((1<<25) - 1);
        char term_text[64];
        corp.lookup(term_id, term_text, sizeof(term_text));
        printf("term '%s', id %d, doc %d, offs %d\n", term_text, term_id, doc_id, term_off);
    }

    return 0;
}
