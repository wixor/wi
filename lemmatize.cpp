#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "fileio.h"
#include "corpus.h"

/* term id: 25 bits -- up to 32 mln terms (real: ??)
 * document id: 20 bits -- up to 1 mln documents (real: 800 k)
 * offset in document: 19 bits -- up to .5 mln terms/document (real max: 50 k)
 */

int main(void)
{
    FileMapping mosquare("db/mosquare");
    Reader rd(mosquare.data(), mosquare.size());
    if(rd.read_u32() != 0x51534f4d) abort();
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
        static uint64_t buffer[1048576*2];
        int n_read = fread(buffer, sizeof(uint64_t), 1048576*2, stdin);
        if(n_read == 0) break;
        for(int i=0; i<n_read; i++)
        {
            uint64_t x = buffer[i];
            int term_id = x >> 39;
            assert(term_id < n_words);
            rd.seek(offs[term_id]);

            int t = rd.read_u32();
            assert((t & 0xffffff) == term_id);
            int n_edges = t>>24;
            assert(n_edges > 0);
            while(n_edges--) {
                int edge = rd.read_u32();
                uint64_t y = (x & 0x7fffffffffLL) | ((uint64_t)edge << 39);
                fwrite_unlocked(&y, sizeof(uint64_t), 1, stdout);
            }
        }
    }

    return 0;
}
