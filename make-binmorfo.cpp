#include <assert.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "bufrw.h"
#include "fileio.h"
#include "corpus.h"

struct trm {
    int term_id;
    int offs, length;
    inline bool operator<(const trm &t) const {
        return term_id < t.term_id;
    }
};

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "make-binmorfo [morfo file]\n");
        return 1;
    }
    FileMapping morfo(argv[1]);
    Corpus corp("db/corpus");

    std::vector<struct trm> terms;
    std::vector<int> bases;

    const char *p   = (const char *)morfo.data(),
               *end = (const char *)morfo.end();
    while(p < end)
    {
        const char *endl = (const char *)memchr(p, '\n', end-p);
        assert(endl);

        const char *q = (const char *)memchr(p, ' ', end-p);
        assert(q);

        struct trm trm;
        trm.term_id = corp.lookup(p, q-p);
        trm.offs = bases.size();
        trm.length = 0;
        assert(trm.term_id != -1);

        terms.push_back(trm);

        while((p = q+1) < endl)
        {
            q = (const char *)memchr(p, ' ', endl-p);
            if(!q) q = endl;

            int base_id = corp.lookup(p, q-p);
            assert(base_id != -1);
            if(base_id == trm.term_id) {
                //fprintf(stderr, "warning: i'm my own base, id %d, word '%.*s'\n",
                //        base_id, q-p, p);
                continue;
            }

            bases.push_back(base_id);
            terms.back().length++;
        }
    }

    std::sort(terms.begin(), terms.end());

    Writer wr;
    wr.write_u32(0x46524f4d);
    wr.write_u32(corp.size());
    for(int i=0, j=0; i<corp.size(); i++) {
        if(j >= terms.size() || terms[j].term_id != i) {
            wr.write_u32(i);
            continue;
        }
        wr.write_u32(i | (terms[j].length << 24));
        for(int k=terms[j].offs; k<terms[j].offs+terms[j].length; k++)
            wr.write_u32(bases[k]);
        j++;
    }

    FileIO binmorfo("db/morfologik", O_WRONLY|O_CREAT|O_TRUNC);
    binmorfo.write_raw(wr.buffer(), wr.tell());

    return 0;
}
