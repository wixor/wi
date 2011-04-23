#include <stdio.h>
#include "corpus.h"

int main(void)
{
    Corpus corp;
    corp.read("corpus");
    static char buf[1024];
    while(fgets(buf, 1023, stdin)) 
        printf("%d\n", corp.lookup(buf, strlen(buf)-1));
    return 0;
}

