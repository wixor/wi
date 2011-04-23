#include <stdio.h>
#include <time.h>
#include "corpus.h"

int main(void)
{
    Corpus corp;
    corp.read("corpus");

    static char buf[1024];
    while(fgets(buf, 1023, stdin))
    {
        struct timespec start, end;

        clock_gettime(CLOCK_MONOTONIC, &start);
        int idx = corp.lookup(buf, strlen(buf)-1);
        clock_gettime(CLOCK_MONOTONIC, &end);

        int time =
            (end.tv_nsec - start.tv_nsec) +
            (end.tv_sec - start.tv_sec) * 1000000000;
        printf("idx: %d, time: %d nsec\n", idx, time);
    }
    return 0;
}

