#include <talloc.h>
#include "bufrw.h"
#include "term.h"

bool TermReader::isWhitespace(int c)
{
    static const char stopchars[] =
        " \n\t\r`~!@#$%^&*-_=+\[]{};':,./<>?";
    for(const char *p = stopchars; *p; p++)
        if(c == *p)
            return true;
    return false;
}
bool TermReader::isTermSeparator(int c) {
    return isWhitespace(c) || c == '|' || c == ')' || c == '(' || c == '"';
}

bool TermReader::eatSymbol(int sym)
{
    size_t start = tell();

    while(!eof())
    {
        int c = read_utf8();
        if(c == sym)
            return true;
        if(!isWhitespace(c))
            break;
    }

    seek(start);
    return false;
}

int TermReader::eatWhitespace()
{
    int count = 0;
    while(!eof()) {
        size_t where = tell();
        int c = read_utf8();
        if(!isWhitespace(c)) {
            seek(where);
            break;
        }
        count++;
    }
    return count;
}

char *TermReader::readTerm(void *memctx)
{
    eatWhitespace();

    size_t start = tell(), end = start;
    while(!eof()) {
        end = tell();
        int c = read_utf8();
        if(isTermSeparator(c))
            break;
    }
    seek(end);

    if(start == end)
        return NULL;

    size_t len = end-start;
    char *term = (char *)talloc_size(memctx, len+1);
    assert(term);

    rd.read_raw_at(start, term, len);
    term[len] = '\0';

    return term;
}

/* -------------------------------------------------------------------------- */

int TermHasher::hash(const char *term, size_t len) const
{
    const int m = 2012345669;
    const char *end = term + len;

    int s = 0;
    while(term < end)
        s = (256LL*s + 1LL*(uint8_t)(*term++)) % m;
    return ((1LL*a*s + 1LL*b) % m) % n;
}
