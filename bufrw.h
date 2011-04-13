#ifndef __BUFRW_H__
#define __BUFRW_H__

#include <stdlib.h> /* for ssize_t */
#include <stdio.h> /* for SEEK_* */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <endian.h>

/*
 * Writer: write to auto-growing memory buffer
 *
 * Writer creates an initially-empty memory buffer and allows appending
 * data to it. When the buffer runs out of space, it is automatically
 * enlarged. Writer will never destroy the buffer: user is supposed to
 * fetch buffer's address when done writing and free it when convinient.
 * Buffer address becomes invalid upon any write operation; also no write
 * must be requested once user has modified the buffer.
 *
 * API summary:
 * - buffer: return the underlying buffer
 * - tell: how many bytes have been written
 * - skip: move current position forward without writing any data
 * - write_raw: write raw data at current position
 * - write_u[8/16/24/32]: write little-endian unsigned integer of given size
 * - write_uv: write unsigned integer using variable length encoding
 * - write_utf8: write integer using utf-8 encoding
 * - align: move current position forward so that is is suitably aligned
 */
class Writer
{
    /* buf = beginning of the buffer,
     * ptr = current position,
     * end = end of the buffer (first address past the last buffer byte
     *
     * it holds that buffer size = end - buf and buf <= ptr <= end. */
    char *buf, *ptr, *end;

    /* enlarge buffer so that 'needed' bytes can be appended */
    void grow(size_t needed);

public:
    /* create Writer; 'len' is initial buffer size */
    Writer(size_t len=128) : buf(NULL), ptr(NULL), end(NULL) { grow(len); }

    inline const void* buffer() const { return buf; }
    inline void* buffer() { return buf; }
    inline size_t tell() const { return ptr - buf; }

    inline void skip(size_t size)
    {
        if(__builtin_expect(ptr + size > end, 0))
            grow(size);
        memset(ptr, 0, size);
        ptr += size;
    }

    inline void write_raw(const void *buf, size_t size)
    {
        if(__builtin_expect(ptr + size > end, 0))
            grow(size);
        memcpy(ptr, buf, size);
        ptr += size;
    }

    inline void write_u8(uint8_t v)   { write_raw(&v, sizeof(v)); }
    inline void write_u16(uint16_t v) { v = htole16(v); write_raw(&v, sizeof(v)); }
    inline void write_u24(uint32_t v) { write_u8(v); write_u16(v>>8); }
    inline void write_u32(uint32_t v) { v = htole32(v); write_raw(&v, sizeof(v)); }

    void write_uv(uint32_t v);
    void write_utf8(uint32_t v);
    
    inline void align(int boundary)
    {
        int rem = tell() % boundary;
        if(rem)
            skip(boundary - rem);
    }
};

/* -------------------------------------------------------------------------- */

/*
 * Reader: read from memory buffer
 *
 * Reader takes a memory buffer and allows reading data from it. The passed
 * buffer must stay valid while Reader is used; Reader however will never
 * modify the buffer. Upon any read error (such as truncated or malformed data)
 * or when trying to seek past buffer boundaries, program will terminate.
 * Reads of integers return uint32_t regardless of the actual integer size
 * being read; this helps avoiding nasty bugs in user code.
 *
 * API summary:
 * - seek: move current position around
 * - tell: report current position relative to buffer start
 * - eof: check if current position is at end of buffer
 * - read_raw: read raw data from current position
 * - read_u[8/16/24/32]: read little-endian unsigned integer of given size
 * - read_uv: read unsigned integer using variable length encoding
 * - read_utf8: read integer using utf-8 encoding
 * - align: move current position forward so that is is suitably aligned
 */
class Reader
{
    /* buf = beginning of the buffer,
     * ptr = current position,
     * end = end of the buffer (first address past the last buffer byte
     *
     * it holds that buffer size = end - buf and buf <= ptr <= end. */
    const char *buf, *ptr, *end;

public:
    /* create Reader; buffer is assumed to start at 'buf' and be 'len' bytes long */
    Reader(const void *buf, size_t len) :
        buf((const char *)buf), ptr((const char *)buf), end((const char *)buf+len) { }

    inline void seek(ssize_t offs, int whence)
    {
        switch(whence) {
            case SEEK_CUR: ptr += offs; break;
            case SEEK_END: ptr = end-offs; break;
            case SEEK_SET: ptr = buf+offs; break;
            default: assert(!"unknown whence"); break;
        }
        assert(buf <= ptr && ptr <= end);
    }
    inline size_t tell() const { return ptr - buf; }
    inline bool eof() const { return ptr >= end; }

    inline void read_raw(void *buf, size_t size)
    {
        assert(__builtin_expect(ptr+size <= end, 1));
        memcpy(buf, ptr, size);
        ptr += size;
    }

    inline uint32_t read_u8()  { uint8_t ret;  read_raw(&ret, sizeof(ret)); return ret; }
    inline uint32_t read_u16() { uint16_t ret; read_raw(&ret, sizeof(ret)); return le16toh(ret); }
    inline uint32_t read_u24() { return read_u8() | (read_u16() << 8); }
    inline uint32_t read_u32() { uint32_t ret; read_raw(&ret, sizeof(ret)); return le32toh(ret); }
    
    uint32_t read_uv();
    uint32_t read_utf8();
    
    inline void align(int boundary)
    {
        int rem = tell() % boundary;
        if(rem)
            seek(boundary - rem, SEEK_CUR);
    }
};

#endif
