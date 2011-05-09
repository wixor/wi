#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>

static const uint64_t *data;
struct finger {
    int p, end;
    inline bool operator<(const finger &f) const { return data[p] > data[f.p]; }
};

template<typename T>
static T divup(T a, T b) { return (a+b-1)/b; }

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "digisort [digitize file]\n");
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if(fd == -1) {
        fprintf(stderr, "failed to open file: %m");
        abort();
    }

    off_t size = lseek(fd, 0, SEEK_END);
    assert(size != (off_t)-1);

    int chunk_size = 32*1048576;
    int chunk_count = std::max((int)divup(size, (off_t)(chunk_size*sizeof(uint64_t))), 1);
    finger fingers[chunk_count];
    fingers[0].p = 0;
    fingers[0].end = chunk_size;
    for(int i=1; i<chunk_count; i++) {
        fingers[i].p = fingers[i-1].end;
        fingers[i].end = fingers[i].p+chunk_size;
    }
    fingers[chunk_count-1].end = size / sizeof(uint64_t);

    fprintf(stderr, "chunks: %d\n", chunk_count);

    {
        uint64_t *sortbuf = (uint64_t *)malloc(sizeof(uint64_t) * chunk_size);
        for(int i=0; i<chunk_count; i++)
        {
            fprintf(stderr, "sorting chunk %d\n", i+1);
            pread(fd, sortbuf,
                  (fingers[i].end - fingers[i].p)*sizeof(uint64_t),
                  fingers[i].p * sizeof(uint64_t));
            std::sort(sortbuf, sortbuf+(fingers[i].end - fingers[i].p));
            pwrite(fd, sortbuf,
                  (fingers[i].end - fingers[i].p)*sizeof(uint64_t),
                  fingers[i].p * sizeof(uint64_t));
        }
        free(sortbuf);
    }

    data = (const uint64_t *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(data);

    setvbuf(stdout, (char *)malloc(1048576*16), _IOFBF, 1048576*16);

    fprintf(stderr, "merging\n");
    std::make_heap(fingers, fingers+chunk_count);
    int term_cnt = 0;
    while(chunk_count)
    {
        std::pop_heap(fingers, fingers+chunk_count);
        finger *f = fingers + (chunk_count-1);

        fwrite_unlocked(data + f->p, sizeof(uint64_t), 1, stdout);
        if(++term_cnt % 100000 == 0)
            fprintf(stderr, "written %d terms\r", term_cnt);

        if(++f->p < f->end)
            std::push_heap(fingers, fingers+chunk_count);
        else
            chunk_count--;
    }
    fprintf(stderr, "\n");

    return 0;
}

