#include <stdio.h>
#include "corpus.h"

int main(void)
{
    Corpus corp;
    corp.read("db/corpus");

    static char buf[1024];
    while(fgets(buf, 1023, stdin))
    {
        if(buf[0] == '#') {
            int idx = strtol(buf+1, NULL, 0);
            corp.lookup(idx, buf, 1023);
            printf("word: '%s'\n", buf);
        } else {
            int idx = corp.lookup(buf, strlen(buf)-1);
            printf("idx: %d\n", idx);
        }
    }
    return 0;
}

