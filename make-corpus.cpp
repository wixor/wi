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

    inline int cmp(const word &w) const
    {
        if(likely(hash != w.hash))
            return hash - w.hash;
        for(int i=0;;i++) {
            if(data[i] != w.data[i])
                return data[i] - w.data[i];
            if(unlikely(data[i] == ' ' || data[i] == '\n'))
                return 0;
        }
    }

    inline bool operator<(const word &w) const {
        return cmp(w) < 0;
    }
    inline bool operator==(const word &w) const {
        return cmp(w) == 0;
    }

    inline int length() const {
        for(int i=0; ; i++)
            if(data[i] == ' ' || data[i] == '\n')
                return i;
    }
};

static TermHasher th;
static std::vector<word> words, result;
static int uniqness_tolerance = 1000000;

static void lost_uniqness()
{
    if(--uniqness_tolerance > 0)
        return;
    uniqness_tolerance = 1000000;

    printf("sorting\n");
    std::sort(words.begin(), words.end());

    printf("merging\n");
    std::vector<word> v;
    v.reserve(words.size() + result.size());

    unsigned int i=0, j=0;
    while(i < result.size() && j < words.size())
    {
        while(j+1 < words.size() && words[j] == words[j+1]) j++;

        int c = result[i].cmp(words[j]);

        v.push_back(c < 0 ? result[i] : words[j]);

        if(c <= 0) i++;
        if(c >= 0) j++;
    }
    while(i < result.size()) v.push_back(result[i++]);
    while(j < words.size()) {
        while(j+1 < words.size() && words[j] == words[j+1]) j++;
        v.push_back(words[j++]);
    }

    words.clear();
    std::swap(v, result);
}

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
            lost_uniqness();
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
   
    uniqness_tolerance = 0;
    lost_uniqness();

    printf("distinct words: %d\n", (int)result.size());
    for(int i=1; i<(int)result.size(); i++)
        assert(result[i-1] < result[i]);

    Writer wr;

    printf("writing header\n");
    wr.write_u32(0x50524f43);
    wr.write_u32(th.a);
    wr.write_u32(th.b);
    wr.write_u32(th.n);

    printf("writing bucket sizes\n");
    for(int i=0,p=0; i<th.buckets(); i++) {
        int q = p;
        while(p<(int)result.size() && result[p].hash == i) p++;
        assert(p-q < 256);
        wr.write_u8(p-q);
    }

    printf("writing word lengths\n");
    for(int i=0; i<(int)result.size(); i++)
        wr.write_u8(result[i].length());

    printf("writing result\n");
    for(int i=0; i<(int)result.size(); i++)
        wr.write_raw(result[i].data, result[i].length());

    printf("flushing\n");
    FileIO fio("db/corpus", O_WRONLY|O_CREAT|O_TRUNC);
    fio.write_raw(wr.buffer(), wr.tell());

    return 0;
}


