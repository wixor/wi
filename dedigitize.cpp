#include <stdio.h>
#include "fileio.h"
#include "corpus.h"
#include "rawpost.h"

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "dedigitize [digitize output]\n");
        return 1;
    }

    Corpus corp("db/corpus");
    FileMapping fmap(argv[1]);
    const uint64_t *p   = (const uint64_t *)fmap.data(),
                   *end = (const uint64_t *)fmap.end();
    while(p < end)
    {
        rawpost x(*p++);
        char term_text[256+16];
        corp.lookup(x.term_id(), term_text, sizeof(term_text));
        printf("term '%s', id %d, doc %d, offs %d\n",
                term_text, x.term_id(), x.doc_id(), x.term_off());
    }

    return 0;
}
