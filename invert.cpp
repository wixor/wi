#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>

static const int chunk = 1<<25; /* 32m entries -> 256 mb */

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "digisort [digitize file]\n");
        return 1;
    }

    off_t size;
    uint64_t *buf, *end;
    {
        int fd = open(argv[1], O_RDWR);
        if(fd == -1) {
            fprintf(stderr, "failed to open file: %m");
            abort();
        }

        size = lseek(fd, 0, SEEK_END);
        assert(size != (off_t)-1);
        buf = (uint64_t *)mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        assert(buf != NULL);
        end = (uint64_t *)((char *)buf + size);

        close(fd);
    }

    std::stable_sort(buf, end); 

    return 0;
}

