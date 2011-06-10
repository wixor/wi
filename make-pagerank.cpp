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
#include "fileio.h"

#define ALPHA (0.1f)
#define CONVERGENCE (0.001f)

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

static void read_artitles()
{
    FileIO fio("db/artitles", O_RDONLY);
    uint32_t hdr[2];
    fio.read_raw(hdr, sizeof(hdr));
    assert(hdr[0] == 0x4c544954);
    n_articles = hdr[1];
    fio.close();
}

static void read_edges()
{
    int alloc_edges = 1024, u, v;

    outdeg = talloc_array(NULL, int, n_articles);
    edges = talloc_array(NULL, edge, alloc_edges);
    assert(outdeg && edges);

    memset(outdeg, 0, sizeof(int)*n_articles);

    while(scanf("%d %d", &u, &v) == 2) {
        if(n_edges == alloc_edges) {
            alloc_edges *= 2;
            edges = talloc_realloc(NULL, edges, edge, alloc_edges);
            assert(edges);
        }
        edges[n_edges].from = u;
        edges[n_edges].to = v;
        outdeg[u] ++;
        n_edges++;
    }

    std::sort(edges, edges+n_edges);
}

static float run_pagerank(const float *src, float *dst)
{
    float global = 0.f;
    for(int i=0; i<n_articles; i++)
        global += outdeg[i]
                    ? src[i] * ALPHA/(float)n_articles
                    : src[i] / (float)n_articles;

    floatset(dst, global, n_articles);

    for(int i=0, j; i<n_edges; i = j) {
        float x = src[edges[i].from] * (1.f - ALPHA) / (float)outdeg[edges[i].from];
        for(j = i; j<n_edges && edges[j].from == edges[i].from; j++)
            dst[edges[j].to] += x;
    }

    float diff = 0.f;
    for(int i=0; i<n_articles; i++)
        diff += fabsf(src[i] - dst[i]);
    return diff / (float)n_articles;
}

static void compute_pagerank()
{
    pagerank = talloc_array(NULL, float, n_articles);
    float *temp = talloc_array(NULL, float, n_articles);
    assert(pagerank && temp);

    floatset(pagerank, 1.f/(float)n_articles, n_articles);

    int step_count = 0;
    float diff = 0.f;
    do {
        printf("step %d, error: %e\r", ++step_count, diff);
        diff = run_pagerank(pagerank, temp);
        std::swap(pagerank, temp);
    } while(diff > CONVERGENCE);

    putchar('\n');
}

static void write_artitles()
{
    FileIO fio("db/artitles", O_WRONLY);
    fio.write_raw(pagerank, sizeof(float)*n_articles, 8 + 4*n_articles);
    fio.close();
}

int main(void)
{
    read_artitles();
    read_edges();
    compute_pagerank();
    write_artitles();

    return 0;
}

