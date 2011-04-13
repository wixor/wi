#include <stdlib.h>
#include "bufrw.h"

void Writer::grow(size_t need)
{
    size_t size = end - buf,
           left = end - ptr;

    if(size == 0) 
        size = left = 1;

    while(left < need) {
        left += size;
        size += size;
    }

    buf = (char *)realloc(buf, size);
    assert(buf);

    end = buf + size;
    ptr = end - left;
}

void Writer::write_uv(uint32_t v)
{
    if(v >= 0x10000000) 
        write_u8((v >> 28) | 0x80);
    if(v >= 0x00200000) 
        write_u8((v >> 21) | 0x80);
    if(v >= 0x00004000) 
        write_u8((v >> 14) | 0x80);
    if(v >= 0x00000080) 
        write_u8((v >> 7)  | 0x80);
    write_u8(v & 0x7F);
}

void Writer::write_utf8(uint32_t v)
{
    if(v <= 0x0000007F)
        write_u8(v);
    else if(v <= 0x000007FF)
        write_u8(v>>6  & 0x1F | 0xC0);
    else if(v <= 0x0000FFFF)
        write_u8(v>>12 & 0x0F | 0xE0);
    else if(v <= 0x001FFFFF)
        write_u8(v>>18 & 0x07 | 0xF0);
    else if(v <= 0x03FFFFFF)
        write_u8(v>>24 & 0x03 | 0xF8);
    else if(v <= 0x7FFFFFFF)
        write_u8(v>>30 & 0x1  | 0xFC);
    else
        assert(!"value too large for utf-8");

    if(v > 0x03FFFFFF)
        write_u8(v >> 24 & 0x3F | 0x80);
    if(v > 0x001FFFFF)
        write_u8(v >> 18 & 0x3F | 0x80);
    if(v >= 0x0000FFFF)
        write_u8(v >> 12 & 0x3F | 0x80);
    if(v > 0x000007FF)
        write_u8(v >> 6  & 0x3F | 0x80);
    if(v > 0x0000007F)
        write_u8(v >> 0  & 0x3F | 0x80);
}

uint32_t Reader::read_uv()
{
    uint32_t ret = 0, x;
    do {
        x = read_u8();
        ret = (ret << 7) | (x & 0x7F);
    } while(x & 0x80);
    return ret;
}

uint32_t Reader::read_utf8()
{
    uint32_t ret = read_u8();
    int cnt; /* # of additional bytes */

    if((ret & 0x80) == 0x00)
        cnt = 0;
    else if((ret & 0xE0) == 0xC0)
        cnt = 1, ret &=~ 0xC0;
    else if((ret & 0xF0) == 0xE0)
        cnt = 2, ret &=~ 0xE0;
    else if((ret & 0xF8) == 0xF0)
        cnt = 3, ret &=~ 0xF0;
    else if((ret & 0xFC) == 0xF8)
        cnt = 4, ret &=~ 0xF8;
    else if((ret & 0xFE) == 0xFC)
        cnt = 5, ret &=~ 0xFC;
    else
        assert(!"invalid utf-8 (first byte)");

    while(cnt--) {
        uint8_t c = read_u8();
        assert((c & 0xC0) == 0x80);
        ret = (ret << 6) | (c & 0x3F);
    }

    return ret;
}
