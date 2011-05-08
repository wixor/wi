#include "bufrw.h"
#include "fileio.h"
#include <vector>
#include <algorithm>

int main(void)
{
    FileMapping morf("db/morfologik");
    Reader rd(morf.data(), morf.size());
    assert(rd.read_u32() == 0x46524f4d);
    int n_words = rd.read_u32();

    std::vector<long long> v;

    for(int i=0; i<n_words; i++)
    {
        int t = rd.read_u32();
        assert((t & 0xffffff) == i);
        int n_bases = t>>24;

        v.push_back(((long long)i << 32) | i);

        while(n_bases--) {
            int base = rd.read_u32();
            v.push_back(((long long)base << 32) | i);
        }
    }

    std::sort(v.begin(), v.end());

    std::vector<int> offs(n_words, -1), lens(n_words, 0);
    for(int i=0; i<v.size(); i++) {
        int x = v[i]>>32;
        if(offs[x] == -1) offs[x] = i;
        lens[x]++;
    }

    FileMapping ali("db/aliases");
    Reader alird(ali.data(), ali.size());
    assert(alird.read_u32() == 0x41494c41);
    assert(alird.read_u32() == n_words);
    const int *aliases = (const int *)alird.buffer() + 2;

    std::vector<int> e;

    Writer wr;
    wr.write_u32(0x51534f4d);
    wr.write_u32(n_words);
    rd.seek(8);

    for(int i=0; i<n_words; i++)
    {
        int t = rd.read_u32();
        assert((t & 0xffffff) == i);
        int n_bases = t>>24;

        e.clear();

        for(int k=offs[i]; k<offs[i]+lens[i]; k++)
            if(aliases[(int)v[k]] == (int)v[k])
                e.push_back((int)v[k]);

        while(n_bases--) {
            int base = rd.read_u32();
            for(int k=offs[base]; k<offs[base]+lens[base]; k++)
                if(aliases[(int)v[k]] == (int)v[k])
                    e.push_back((int)v[k]);
        }

        for(int j=0; j<e.size(); )
        {
            bool dupli = false;
            for(int k=0; k<j; k++)
                if(e[k] == e[j]) {
                    dupli = true;
                    break;
                }

            if(dupli) {
                e[j] = e.back();
                e.pop_back();
            } else
                j++;
        }

/*        printf("term %d: %d edges\n", i, (int)e.size());
        for(int j=0; j<e.size(); j++)
            printf("  %d\n", e[j]);*/
        assert(e.size() < 256);
        wr.write_u32(i | (e.size() << 24));
        for(int j=0; j<e.size(); j++)
            wr.write_u32(e[j]);
    }
    
    FileIO mosquare("db/mosquare", O_WRONLY|O_CREAT|O_TRUNC);
    mosquare.write_raw(wr.buffer(), wr.tell());

    return 0;
}

