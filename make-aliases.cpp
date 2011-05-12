#include <stdlib.h>
#include <string.h>
#include "bufrw.h"
#include "fileio.h"

int main(void)
{
    FileMapping morf("db/morfologik");
    Reader rd(morf.data(), morf.size());
    if(rd.read_u32() != 0x46524f4d) abort();
    int n_words = rd.read_u32();

    Writer wr;
    wr.write_u32(0x41494c41);
    wr.write_u32(n_words);

    int alcnt = 0;

    int *v = (int *)malloc(sizeof(int) * n_words);
    memset(v, -1, sizeof(int) * n_words);

    for(int i=0; i<n_words; i++)
    {
        int t = rd.read_u32();
        assert((t & 0xffffff) == i);
        int n_bases = t>>24;

        if(n_bases > 1) {
            wr.write_u32(i);
            while(n_bases--)
                rd.read_u32();
        } else {
            int base = n_bases ? rd.read_u32() : i;
            if(v[base] == -1)
                v[base] = i;
            else
                alcnt++;
            wr.write_u32(v[base]);
        }
    }

    printf("total alias count: %d\n", alcnt);
    
    FileIO aliases("db/aliases", O_WRONLY|O_CREAT|O_TRUNC);
    aliases.write_raw(wr.buffer(), wr.tell());

    return 0;
}

