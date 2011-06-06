#ifndef __RAWPOST_H__
#define __RAWPOST_H__

#include <assert.h>
#include <stdint.h>

/* term id: 25 bits -- up to 32 mln terms (real: ??)
 * document id: 20 bits -- up to 1 mln documents (real: 800 k)
 * offset in document: 19 bits -- up to .5 mln terms/document (real max: 50 k)
 */

struct rawpost
{
    uint64_t raw;

    inline rawpost() { }
    inline rawpost(uint64_t raw) : raw(raw) { }
    inline rawpost(int term_id, int doc_id, int term_pos)
    {
        assert(term_id >= 0 && term_id < (1<<25));
        assert(doc_id >= 0 && doc_id < (1<<20));
        assert(term_pos >= 0 && term_pos < (1<<19));
        raw = ((uint64_t)term_id<<39) | ((uint64_t)doc_id<<19) | term_pos;
    }
    inline rawpost& operator=(uint64_t raw) { this->raw = raw; return *this; }
    inline operator uint64_t() const { return raw; }

    inline int term_id() const { return raw >> 39; }
    inline int doc_id() const { return (raw >> 19) & 0xfffff; }
    inline int term_pos() const { return raw & 0x7ffff; }
};

#endif

