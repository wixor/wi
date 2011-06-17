#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
extern "C" {
#include <talloc.h>
}
#include <algorithm>
#include "likely.h"
#include "bufrw.h"
#include "fileio.h"

#define ALPHA (0.1f)
#define CONVERGENCE (1e-4f)

compile_time_assert(sizeof(wchar_t) == sizeof(float));
union floatwchar {
    float f;
    wchar_t w;
};
static inline void floatset(float *mem, float val, int count) {
    floatwchar u;
    u.f = val;
    wmemset((wchar_t *)mem, u.w, count);
}

struct edge {
    int from, to;
    inline bool operator<(const edge &e) const {
        return from != e.from ? from < e.from : to < e.to;
    }
};

static int n_articles;
static int n_edges;
static int *outdeg;
static edge *edges;
static float *pagerank;

static void read_links()
{
    FileMapping fmap("db/wikilinks");
    Reader rd(fmap.data(), fmap.size());
    rd.assert_u32(0x4b4e494c);
    n_articles = rd.read_u32();
    
    int alloc_edges = 1024;

    outdeg = talloc_array(NULL, int, n_articles);
    edges = talloc_array(NULL, edge, alloc_edges);
    assert(outdeg && edges);

    memset(outdeg, 0, sizeof(int)*n_articles);

    for(int u=0; u<n_articles; u++)
    {
        outdeg[u] = rd.read_u16();
        for(int i=0; i<outdeg[u]; i++)
        {
            int v = rd.read_u32();

            if(n_edges == alloc_edges) {
                alloc_edges += 10 * 1048576; // 10 * 2^20
                edges = talloc_realloc(NULL, edges, edge, alloc_edges);
                assert(edges);
            }
            edges[n_edges].from = u;
            edges[n_edges].to = v;
            n_edges++;
        }
    }

    assert(rd.eof());

    printf("articles: %d, edges: %d (alloc %d)\n",
           n_articles, n_edges, alloc_edges);

    std::sort(edges, edges+n_edges);
}

static float sum(const float *src, float *tmp, int n)
{
    if(n == 0) return 0.f;
    if(n == 1) return src[0];

    for(int i=0; i<n/2; i++) 
        tmp[i] = src[i] + src[n/2+i];
    if(n % 2) tmp[n/2] = src[n-1];

    return sum(tmp, tmp, (n+1)/2);
}

static float run_pagerank(const float *src, float *dst)
{
    for(int i=0; i<n_articles; i++)
        dst[i] = outdeg[i]
                    ? src[i] * ALPHA/(float)n_articles
                    : src[i] / (float)n_articles;
    float global = sum(dst, dst, n_articles);
    floatset(dst, global, n_articles);

    for(int i=0, j; i<n_edges; i = j) {
        float x = src[edges[i].from] * (1.f - ALPHA) / (float)outdeg[edges[i].from];
        for(j = i; j<n_edges && edges[j].from == edges[i].from; j++)
            dst[edges[j].to] += x;
    }

    float err = 0.f;
    for(int i=0; i<n_articles; i++)
        err = std::max(err, fabsf(src[i] - dst[i])/(src[i]+1e-15f));
    return err;
}

static void compute_pagerank()
{
    pagerank = talloc_array(NULL, float, n_articles);
    float *temp = talloc_array(NULL, float, n_articles);
    assert(pagerank && temp);

    floatset(pagerank, 1.f/(float)n_articles, n_articles);

    printf("iterating...\n");

    int step_count = 0;
    float diff = 0.f;
    do {
        diff = run_pagerank(pagerank, temp);
        std::swap(pagerank, temp);
        float s = sum(pagerank, temp, n_articles);

        printf("step %d, error: %e, sum: %.9f\n", ++step_count, diff, s);
    } while(diff > CONVERGENCE);
}

static void write_artitles()
{
    FileIO fio("db/artitles", O_RDWR);

    uint32_t hdr[2];
    fio.read_raw(hdr, sizeof(hdr));
    assert(hdr[0] == 0x4c544954);
    int n = hdr[1];

    printf("saving result, articles in wiki: %d\n", n);

    for(int i=0; i<n_articles; i++)
        pagerank[i] = logf(1.f+pagerank[i]*n_articles);

    fio.write_raw(pagerank, sizeof(float)*n, 8 + 4*n);
    fio.close();
}

int main(void)
{
    read_links();
    compute_pagerank();
    write_artitles();

    return 0;
}

