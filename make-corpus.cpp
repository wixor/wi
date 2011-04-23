#include <assert.h>
#include <vector>
#include <algorithm>

#include "likely.h"
#include "bufrw.h"
#include "fileio.h"
#include "term.h"

struct word {
    int hash;
    const char *data;

    inline bool operator<(const word &w) const
    {
        if(hash != w.hash)
            return hash < w.hash;
        for(int i=0;;i++) {
            if(data[i] != w.data[i])
                return data[i] < w.data[i];
            if(unlikely(data[i] == ' ' || data[i] == '\n'))
                return false;
        }
    }

    inline bool operator==(const word &w) const
    {
        if(hash != hash)
            return false;
        for(int i=0;;i++) {
            if(data[i] != w.data[i])
                return false;
            if(unlikely(data[i] == ' ' || data[i] == '\n'))
                return true;
        }
    }

    inline int length() const {
        for(int i=0; ; i++)
            if(data[i] == ' ' || data[i] == '\n')
                return i;
    }
};

static TermHasher th;
static std::vector<word> words;

static void add_words(const char *start, const char *end)
{
    while(start < end)
    {
        while(start < end && (*start == ' ' || *start == '\n')) start++;
        const char *p = start;
        while(start < end && *start != ' ' && *start != '\n') start++;
        if(start - p > 0) {
            word w;
            w.hash = th(p, start-p);
            w.data = p;
            words.push_back(w);
        }
    }
}

int main(int argc, char *argv[])
{
    th.a = 123456337;
    th.b = 203451187;
    th.n = 1000033;

    FileMapping maps[argc-1];

    for(int i=1; i<argc; i++)
        maps[i-1].attach(argv[i]);

    for(int i=0; i<argc-1; i++) {
        printf("reading file %d\n", i+1);
        add_words((const char *)maps[i].data(),
                  (const char *)maps[i].end());
    }
   
    {
      printf("sorting\n");
      std::sort(words.begin(), words.end());
      
      printf("unique-ing\n");
      std::vector<word> unq(words.size());
      int n = std::unique_copy(words.begin(), words.end(), unq.begin()) - unq.begin();
      unq.resize(n);
      std::swap(unq, words);
    }


    printf("distinct words: %d\n", words.size());

    Writer wr;

    printf("writing header\n");
    wr.write_u32(0x50524f43);
    wr.write_u32(th.a);
    wr.write_u32(th.b);
    wr.write_u32(th.n);

    printf("writing bucket sizes\n");
    for(int i=0,p=0; i<th.buckets(); i++) {
        int q = p;
        while(p<words.size() && words[p].hash == i) p++;
        wr.write_u8(p-q);
    }

    printf("writing word lengths\n");
    for(int i=0; i<words.size(); i++)
        wr.write_u8(words[i].length());

    printf("writing words\n");
    for(int i=0; i<words.size(); i++)
        wr.write_raw(words[i].data, words[i].length());

    printf("flushing\n");
    FileIO fio("corpus", O_WRONLY|O_CREAT|O_TRUNC);
    fio.write_raw(wr.buffer(), wr.tell());

    return 0;
}


