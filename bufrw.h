#ifndef __BUFRW_H__
#define __BUFRW_H__

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <endian.h>
#include <unistd.h> /* for SEEK_* */

#include "likely.h"

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
 * - rewind: forget any data written to the buffer
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
     * end = end of the buffer (first address past the last buffer byte)
     *
     * it holds that buffer size = end - buf and buf <= ptr <= end. */
    char *buf, *ptr, *end;

    /* enlarge buffer so that 'needed' bytes can be appended */
    void grow(size_t needed);

public:
    /* create Writer; 'len' is initial buffer size */
    Writer(size_t len=128) : buf(NULL), ptr(NULL), end(NULL) { grow(len); }

    void free();

    inline const void* buffer() const { return buf; }
    inline void* buffer() { return buf; }
    inline size_t tell() const { return ptr - buf; }

    inline void rewind() {
        ptr = buf;
    }

    inline void skip(size_t size)
    {
        if(unlikely(ptr + size > end))
            grow(size);
        memset(ptr, 0, size);
        ptr += size;
    }

    inline void write_raw(const void *v, size_t size)
    {
        if(unlikely(ptr + size > end))
            grow(size);
        memcpy(ptr, v, size);
        ptr += size;
    }

    inline void write_u8(uint8_t v)   { write_raw(&v, sizeof(v)); }
    inline void write_u16(uint16_t v) { v = htole16(v); write_raw(&v, sizeof(v)); }
    inline void write_u24(uint32_t v) { write_u8(v); write_u16(v>>8); }
    inline void write_u32(uint32_t v) { v = htole32(v); write_raw(&v, sizeof(v)); }
    inline void write_u64(uint64_t v) { v = htole64(v); write_raw(&v, sizeof(v)); }

    void write_uv(uint32_t v);
    void write_utf8(int v);

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
 * - attach: specify the buffer to read from
 * - seek: move current position around
 * - tell: report current position relative to buffer start
 * - size: report overall buffer size
 * - eof: check if current position is at end of buffer
 * - buffer: return buffer being read
 * - seek_past: advance current position past the first occurence of given byte
 * - read_raw: read raw data from current position
 * - read_u[8/16/24/32]: read little-endian unsigned integer of given size
 * - assert_u[8/16/24/32]: read integer and assert its value
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
    inline Reader() { }
    inline Reader(const void *bf, size_t len) { attach(bf, len); }
    inline void attach(const void *bf, size_t len) {
        ptr = buf = (const char *)bf;
        end = buf+len;
    }

    inline size_t seek(ssize_t offs, int whence = SEEK_SET)
    {
        switch(whence) {
            case SEEK_CUR: ptr += offs; break;
            case SEEK_END: ptr = end-offs; break;
            case SEEK_SET: ptr = buf+offs; break;
            default: assert(!"unknown whence"); break;
        }
        assert(buf <= ptr && ptr <= end);
        return ptr-buf;
    }
    inline size_t tell() const { return ptr - buf; }
    inline size_t size() const { return end - buf; }
    inline bool eof() const { return ptr >= end; }
    inline const void* buffer() const { return buf; }

    inline size_t seek_past(uint8_t byte)
    {
        const char *p = (const char *)memchr(ptr, byte, end-ptr);
        assert(likely(p));
        ptr = p+1;
        return ptr-buf;
    }

    inline void read_raw_at(size_t offs, void *out, size_t size) const
    {
        assert(likely(buf+offs+size <= end));
        memcpy(out, buf+offs, size);
    }
    inline void read_raw(void *out, size_t size)
    {
        read_raw_at(tell(), out, size);
        ptr += size;
    }

    inline uint32_t read_u8()  { uint8_t ret;  read_raw(&ret, sizeof(ret)); return ret; }
    inline uint32_t read_u16() { uint16_t ret; read_raw(&ret, sizeof(ret)); return le16toh(ret); }
    inline uint32_t read_u24() { return read_u8() | (read_u16() << 8); }
    inline uint32_t read_u32() { uint32_t ret; read_raw(&ret, sizeof(ret)); return le32toh(ret); }
    inline uint64_t read_u64() { uint64_t ret; read_raw(&ret, sizeof(ret)); return le64toh(ret); }

    inline void assert_u8(uint8_t v)   { uint8_t  x = read_u8();  assert(x == v); }
    inline void assert_u16(uint16_t v) { uint16_t x = read_u16(); assert(x == v); }
    inline void assert_u24(uint32_t v) { uint32_t x = read_u32(); assert(x == v); }
    inline void assert_u32(uint32_t v) { uint32_t x = read_u32(); assert(x == v); }
    inline void assert_u64(uint64_t v) { uint64_t x = read_u64(); assert(x == v); }

    uint32_t read_uv();
    int read_utf8();

    inline void align(int boundary)
    {
        int rem = tell() % boundary;
        if(rem)
            seek(boundary - rem, SEEK_CUR);
    }
};

#endif
