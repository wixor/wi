extern "C" {
	#include <talloc.h>
}
#include <algorithm> /* for upper_bound */
#include "bufrw.h"
#include "term.h"

bool TermReader::isQuerySymbol(int c) {
    return c == '|' || c == ')' || c == '(' || c == '"';
}
bool TermReader::isWhitespace(int c) 
{
    static const char stopchars[] =
        " \n\t\r`~!@#$%^&*_=+\\[]{};':,./<>?";
    for(const char *p = stopchars; *p; p++)
        if(c == *p)
            return true;
    if(c >= 0x2000 && c <= 0x206F) /* unicode: general punctuation */
        return true;
    return false;
}
bool TermReader::isTermSeparator(int c) {
    return isWhitespace(c) || isQuerySymbol(c);
}

int TermReader::lowercase(int c) {
    if(c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    switch(c) {
        case 0x0104: /* Ą */
        case 0x0118: /* Ę */
        case 0x015a: /* Ś */
        case 0x0106: /* Ć */
        case 0x0143: /* Ń */
        case 0x0141: /* Ł */
        case 0x017b: /* Ż */
        case 0x0179: /* Ź */
            return c+1;
        case 0x00d3: /* Ó */
            return 0x00f3; 
    }
    return c; /* unknown */
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

char *TermReader::readTerm()
{
    eatWhitespace();

    Writer wr(16);

    size_t end = tell();
    for(;;) {
        if(eof()) break;
        int c = read_utf8();
        if(isTermSeparator(c)) break;
        wr.write_utf8(lowercase(c));
        end = tell();
    }
    seek(end);

    if(wr.tell() == 0) {
        wr.free();
        return NULL;
    }

    return (char *)wr.buffer();
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
