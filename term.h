#ifndef __TERM_H__
#define __TERM_H__

#include <stdlib.h>
#include "bufrw.h"

/*
 * TermReader: read a stream of terms from memory buffer
 *
 * TermReader is a built upon Reader and allows extraction of consecutive terms
 * from given memory buffer. Some of underying Reader's API is exposed.
 * Also requirements for buffer are those of Reader.
 *
 * This facility is used for query parsing, so some characters are considered
 * special. Namely, round parentheses, double quote symbol and pipe symbol
 * are term separators, but are not whitespace.
 *
 * API summary:
 * - attach, seek, tell, eof, read_utf8: as in Reader
 * - eatSymbol: read given symbol from stream, possibly preceded with some
 *      whitespace. if symbol is not found, do nothing (stream position is
 *      not changed)
 * - eatWhitespace: read all whitespace from stream up to first other symbol.
 *      returns number of symbols read.
 * - readTerm: read one term, possibly preceded with some whitespace, create
 *      its copy and return it. stream position is placed just after last
 *      symbol read. memory for copy is allocated with talloc.
 */
class TermReader
{
    Reader rd;

    static inline bool isWhitespace(int c);
    static inline bool isTermSeparator(int c);

public:
    inline void attach(const void *buf, size_t len) {
        rd.attach(buf, len);
    }
    inline off_t seek(ssize_t offs, int whence = SEEK_SET) { return rd.seek(offs, whence); }
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
};


#endif
