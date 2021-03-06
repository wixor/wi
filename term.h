#ifndef __TERM_H__
#define __TERM_H__

#include <stdlib.h>
#include "bufrw.h"

/*
 * TermReader: read a stream of terms from memory buffer
 *
 * TermReader is a built upon Reader and allows extraction of consecutive terms
 * from given memory buffer. Some of underlying Reader's API is exposed.
 * Also requirements for buffer are those of Reader.
 *
 * API summary:
 * - attach, seek, tell, eof, read_utf8: as in Reader
 * - eatSymbol: read given symbol from stream, possibly preceded with some
 *      whitespace. If symbol is not found, do nothing (stream position is
 *      not changed)
 * - eatWhitespace: read all whitespace from stream up to first other symbol.
 *      returns number of symbols read.
 * - readTerm: read one term, possibly preceded with some whitespace, create
 *      its copy and return it. Stream position is placed just after last
 *      symbol read. Memory for copy is allocated with talloc.
 */
class TermReader
{
    Reader rd;

    static inline bool isWhitespace(int c);
    static inline bool isTermSeparator(int c);
    static inline int lowercase(int c);

public:
    inline TermReader() { }
    inline TermReader(const void *buf, size_t len) { attach(buf, len); }

    inline void attach(const void *buf, size_t len) {
        rd.attach(buf, len);
    }
    inline size_t seek(ssize_t offs, int whence = SEEK_SET) { return rd.seek(offs, whence); }
    inline size_t tell() const { return rd.tell(); }
    inline bool eof() const { return rd.eof(); }

    inline int read_utf8() { return rd.read_utf8(); }

    bool eatSymbol(int c);
    int eatWhitespace();
    char *readTerm();
};


struct TermHasher
{
    int a,b, n;
    
    int hash(const char *term, size_t len) const;
    inline int buckets() const { return n; }
    inline int operator()(const char *term, size_t len) const { return hash(term, len); }
};

#endif
