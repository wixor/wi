#ifndef __FILEIO_H__
#define __FILEIO_H__

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
extern "C" {
#include <talloc.h>
}

#include "likely.h"

class FileIO
{
    int fd;

    static void open_error(const char *filename, int flags, mode_t mode) __attribute__((noreturn));
    static void read_error(ssize_t rd, size_t size) __attribute__((noreturn));
    static void write_error(ssize_t wr, size_t size) __attribute__((noreturn));

public:
    inline FileIO() { }
    inline FileIO(int fd) { attach(fd); }
    inline FileIO(const char *filename, int flags, mode_t mode = 0666) { open(filename, flags, mode); }

    inline void attach(int fd) {
        this->fd = fd;
    }
    inline void open(const char *filename, int flags, mode_t mode = 0666) {
        fd = ::open(filename, flags, mode);
        if(unlikely(fd == -1))
            open_error(filename, flags, mode);
    }
    inline void close() {
        ::close(fd);
        fd = -1;
    }

    inline off_t seek(off_t offs, int whence = SEEK_SET) const
    {
        off_t ret = lseek(fd, offs, whence);
        assert(likely(ret != -1));
        return ret;
    }
    inline off_t size() const
    {
        off_t here = tell();
        off_t ret = lseek(fd, 0, SEEK_END);
        assert(likely(ret != -1));
        seek(here);
        return ret;
    }
    inline off_t left() const
    {
        off_t here = tell();
        off_t ret = lseek(fd, 0, SEEK_END);
        assert(likely(ret != -1));
        seek(here);
        return ret-here;
    }
    inline off_t tell() const { return seek(0, SEEK_CUR); }
    inline bool eof() const { return left() == 0; }
    inline int filedes() const { return fd; }

    inline void read_raw(void *out, size_t size, off_t offs = -1) const
    {
        ssize_t rd =
            offs != -1 ? pread(fd, out, size, offs)
                       : read(fd, out, size);
        if(unlikely(rd != (ssize_t)size))
            read_error(rd, size);
    }

    inline void *read_raw_alloc(size_t size, off_t offs = -1) const
    {
        void *buf = talloc_size(NULL, size);
        assert(likely(buf));
        read_raw(buf, size, offs);
        return buf;
    }

    inline void write_raw(const void *v, size_t size, off_t offs = -1) const
    {
        ssize_t wr =
            offs != -1 ? pwrite(fd, v, size, offs)
                       : write(fd, v, size);
        if(unlikely(wr != (ssize_t)size))
            write_error(wr, size);
    }

    inline void align(int boundary) const
    {
        int rem = tell() % boundary;
        if(rem)
            seek(boundary - rem, SEEK_CUR);
    }
};

/* -------------------------------------------------------------------------- */

class FileMapping
{
    void *mapping;
    size_t _size;

    static void mapping_error(int fd, size_t size) __attribute__((noreturn));

public:
    inline FileMapping() { mapping = NULL; _size = 0; }
    inline FileMapping(const char *filename) { mapping = NULL; _size = 0; attach(filename); }
    inline FileMapping(int fd, size_t size) {  mapping = NULL; _size = 0; attach(fd, size); }
    inline ~FileMapping() { destroy(); }

    inline void attach(const char *filename)
    {
        FileIO fio(filename, O_RDONLY);
        size_t size = fio.seek(0, SEEK_END);
        attach(fio.filedes(), size);
        fio.close();
    }
    inline void attach(int fd, size_t size)
    {
        _size = size;
        mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if(unlikely(mapping == NULL))
            mapping_error(fd, size);
    }
    inline void destroy()
    {
        if(mapping)
            munmap(mapping, _size);
        mapping = NULL; _size = 0;
    }

    inline void*  data() const { return mapping; }
    inline void*  end()  const { return (char *)mapping + _size; }
    inline size_t size() const { return _size; }
    inline operator void*() const { return mapping; }
};

#endif

