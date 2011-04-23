#include <stdlib.h>
#include <stdio.h>
#include "fileio.h"

void FileIO::open_error(const char *filename, int flags, mode_t mode)
{
    fprintf(stderr, "opening file '%s' failed: %m\n", filename);
    abort();
}

void FileIO::read_error(ssize_t rd, size_t size)
{
    if(rd == -1)
        fprintf(stderr, "read of %zu bytes failed: %m\n", size);
    else
        fprintf(stderr, "read of %zu bytes failed: only %zd read\n", size, rd);
    abort();
}
void FileIO::write_error(ssize_t wr, size_t size)
{
    if(wr == -1)
        fprintf(stderr, "write of %zu bytes failed: %m\n", size);
    else
        fprintf(stderr, "write of %zu bytes failed: only %zd written\n", size, wr);
    abort();
}

void FileMapping::mapping_error(int fd, size_t size)
{
    fprintf(stderr, "mapping file failed: %m\n");
    abort();
}
