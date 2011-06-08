#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "fileio.h"
#include "corpus.h"
#include "rawpost.h"

int main(void)
{
    FileMapping mosquare("db/mosquare");
    Reader rd(mosquare.data(), mosquare.size());
    rd.assert_u32(0x51534f4d);
    int n_words = rd.read_u32();

    int *offs = (int *)malloc(sizeof(int) * n_words);
    for(int i=0; i<n_words; i++)
    {
        int o = rd.tell();
        
        int t = rd.read_u32();
        assert((t & 0xffffff) == i);
        int n_edges = t>>24;
        rd.seek(n_edges * 4, SEEK_CUR);

        offs[i] = o;
    }

    setvbuf(stdout, (char *)malloc(1048576*16), _IOFBF, 1048576*16);

    for(;;) {
        static rawpost buffer[1048576*2];
        int n_read = fread(buffer, sizeof(rawpost), 1048576*2, stdin);
        if(n_read == 0) break;
        for(int i=0; i<n_read; i++)
        {
            rawpost &x = buffer[i];
            assert(x.term_id() < n_words);

            rd.seek(offs[x.term_id()]);
            
            int t = rd.read_u32();
            assert((t & 0xffffff) == x.term_id());
            int n_edges = t>>24;
            assert(n_edges > 0);

            while(n_edges--) {
                int edge = rd.read_u32();
                rawpost y(edge, x.doc_id(), x.term_pos());
                fwrite_unlocked(&y, sizeof(y), 1, stdout);
            }
        }
    }

    return 0;
}
