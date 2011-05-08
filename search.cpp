#include <time.h>
#include <pthread.h>
#include <sys/eventfd.h>
extern "C" {
#include <talloc.h>
}
#include <algorithm>

#include "bufrw.h"
#include "fileio.h"
#include "term.h"

/* -------------------------------------------------------------------------- */

#define info(fmt, ...) printf(fmt, ## __VA_ARGS__)
//#define info(fmt, ...)
struct QueryNode;
static void dump_query_tree(const QueryNode *node, int indent = 0);

/* -------------------------------------------------------------------------- */

class Timer
{
    struct timespec ts;
    int clock;
public:
    void start(int clock = CLOCK_MONOTONIC);
    double end();
};

void Timer::start(int clock) {
    this->clock = clock;
    assert(clock_gettime(clock, &ts) == 0);
}
double Timer::end()
{
    struct timespec now;
    assert(clock_gettime(clock, &now) == 0);
    return 1e-9*(now.tv_nsec - ts.tv_nsec) +
                (now.tv_sec  - ts.tv_sec);
}

/* -------------------------------------------------------------------------- */

class Artitles
{
    void *memctx;
    char **titles;
public:
    Artitles();
    ~Artitles();
    void read(const char *filename);
    inline const char *lookup(int key) const { return titles[key]; }
};

Artitles::Artitles() {
    memctx = NULL;
}
Artitles::~Artitles() {
    if(memctx) talloc_free(memctx);
}

void Artitles::read(const char *filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "article titles");
    assert(memctx);

    info("reading article titles...\n");

    Timer timer; timer.start();

    FileIO fio(filename, O_RDONLY);
    
    off_t size = fio.seek(0, SEEK_END);
    char *data = (char *)fio.read_raw_alloc(size, 0);
    talloc_steal(memctx, data);

    fio.close();

    Reader rd(data, size);
    
    assert(rd.read_u32() == 0x4c544954);
    int n_articles = rd.read_u32();

    titles = talloc_array(memctx, char *, n_articles);
    assert(titles);
    for(int i=0; i<n_articles; i++) {
        titles[i] = data + rd.tell();
        rd.seek_past('\0');
    }

    info("article titles read in %.03lf seconds.\n", timer.end());
}

static Artitles artitles; /* GLOBAL */

/* -------------------------------------------------------------------------- */

enum IndexType {
    POSITIONAL, LEMMATIZED
};

class Dictionary
{
    struct PostingList {
        off_t offset;
        int n_postings;
    };

    struct Term {
        int lemmatized_list_id;
        int text_length;
        char *text;
    };

    void *memctx;

    TermHasher hasher;
    PostingList *lemmatized, *positional;
    Term *terms, **buckets;

    void do_read(Reader rd);

public:
    struct PostingListInfo {
        off_t offset;
        size_t size;
        int n_postings;
    };

    typedef int Key;

    Dictionary();
    ~Dictionary();

    void read(const char *filename);
    Key lookup(const char *text, size_t len, IndexType idxtype) const;

    inline PostingListInfo getPostingListInfo(Key key) const;
};

Dictionary::Dictionary() {
    memctx = NULL;
}
Dictionary::~Dictionary() {
    if(memctx) talloc_free(memctx);
}

void Dictionary::read(const char *filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "dictionary");
    assert(memctx);

    info("reading dictionary titles...\n");

    Timer timer; timer.start();

    FileMapping fmap(filename);
    do_read(Reader(fmap.data(), fmap.size()));

    info("dictionary read in %.03lf seconds\n", timer.end());
}

void Dictionary::do_read(Reader rd)
{
    /* check magic value */
    assert(rd.read_u32() == 0x54434944);

    /* read hash function parameters */
    hasher.a = rd.read_u32();
    hasher.b = rd.read_u32();
    hasher.n = rd.read_u32();

    /* read lemmatized lists count */
    int bucket_count = hasher.buckets(),
        lemmatized_list_count = rd.read_u32();

    /* save bucket size reader for future use */
    Reader bucket_size_rd = rd;

    /* count terms */
    int term_count = 0;
    for(int i=0; i<bucket_count; i++)
        term_count += rd.read_u8();

    /* allocate stuff */
    lemmatized = talloc_array(memctx, PostingList, lemmatized_list_count+1);
    positional = talloc_array(memctx, PostingList, term_count+1);
    buckets = talloc_array(memctx, Term*, bucket_count+1);
    terms = talloc_array(memctx, Term, term_count);
    assert(lemmatized && positional && terms && buckets);

    { /* read bucket info */
        Term *t = terms;
        for(int i=0; i<bucket_count; i++)
        {
            buckets[i] = t;
            int bucket_size = bucket_size_rd.read_u8();
            while(bucket_size--) {
                t->lemmatized_list_id = rd.read_u24();
                t->text_length = rd.read_u8();
                t++;
            }
        }
        buckets[bucket_count] = t;
    }

    /* read positional list info */
    positional[0].offset = 4;/* because of index file magic number */
    for(int i=1; i<=term_count; i++) {
        positional[i].offset = positional[i-1].offset + rd.read_uv();;
        positional[i-1].n_postings = rd.read_uv();
    }

    /* read lemmatized list info */
    lemmatized[0].offset = 4; /* because of index file magic number */
    for(int i=1; i<=lemmatized_list_count; i++) {
        lemmatized[i].offset = lemmatized[i-1].offset + rd.read_uv();;
        lemmatized[i-1].n_postings = rd.read_uv();
    }

    /* compute total texts length */
    int term_texts_length = 0;
    for(int i=0; i<term_count; i++)
        term_texts_length += terms[i].text_length;

    /* read term texts */
    char *term_texts = (char *)talloc_size(memctx, term_texts_length);
    assert(term_texts);
    rd.read_raw(term_texts, term_texts_length);

    /* bind term texts to terms */
    terms[0].text = term_texts;
    for(int i=1; i<term_count; i++)
        terms[i].text = terms[i-1].text + terms[i-1].text_length;

    /* make sure we read everything */
    assert(rd.eof());
}

Dictionary::Key Dictionary::lookup(const char *text, size_t len, IndexType idxtype) const
{
    int h = hasher.hash(text, len);

    const Term *t = buckets[h], *tend = buckets[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)t->text_length)
            continue;
        if(memcmp(text, t->text, len) != 0)
            continue;
    
        int k = buckets[h] - buckets[0] + i;
        switch(idxtype) {
            case LEMMATIZED: return terms[k].lemmatized_list_id | 0x80000000;
            case POSITIONAL: return k;
        }
        abort();
    }

    return -1;
}

Dictionary::PostingListInfo Dictionary::getPostingListInfo(Key key) const
{
    PostingList *lists = (key & 0x80000000) ? lemmatized : positional;
    int k = key & ~0x80000000;

    PostingListInfo ret;
    ret.offset = lists[k].offset;
    ret.size = lists[k+1].offset - lists[k].offset;
    ret.n_postings = lists[k].n_postings;

    return ret;
}

static Dictionary dictionary; /* GLOBAL */
static long empty_posting_list; /* GLOBAL */

/* -------------------------------------------------------------------------- */

class PostingsSource
{
public:
    struct ReadRq {
        Dictionary::Key term_id;
        void *data;
    };

private:
    void *memctx;
    int evfd_rq, evfd_done;
    int lemmatized_fd, positional_fd;

    pthread_t thread;
    ReadRq *rqs;
    int rqs_count, fd, rqs_left;

    static int openIndex(const char *filename, uint32_t magic);
    static void* runThreadFunc(void *);
    void threadFunc();

public:
    PostingsSource();
    ~PostingsSource();

    void start(const char *positional_filename, const char *lemmatized_filename);
    void stop();

    void request(ReadRq *rqs, int count, IndexType idxtype);
    int wait();
};

void* PostingsSource::runThreadFunc(void *arg) {
    ((PostingsSource*)arg)->threadFunc();
    return NULL;
}

void PostingsSource::threadFunc()
{
    for(;;)
    {
        uint64_t foo;
        eventfd_read(evfd_rq, &foo);

        FileIO fio(fd);
        for(int i=0; i<rqs_count; i++)
        {
            Dictionary::PostingListInfo info =
                dictionary.getPostingListInfo(rqs[i].term_id);
            rqs[i].data = fio.read_raw_alloc(info.size, info.offset);
            talloc_steal(memctx, rqs[i].data);

            eventfd_write(evfd_done, 1);
        }
        fd = -1;
    }
}

PostingsSource::PostingsSource() {
    memctx = NULL;
}
PostingsSource::~PostingsSource() {
    if(memctx) stop();
}

int PostingsSource::openIndex(const char *filename, uint32_t magic)
{
    FileIO fio(filename, O_RDONLY);
    
    uint32_t mg;
    fio.read_raw(&mg, sizeof(mg));
    assert(mg == magic);

    return fio.filedes();
}

void PostingsSource::start(const char *positional_filename, const char *lemmatized_filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "postings reader");
    assert(memctx);
    
    info("starting postings source...\n");

    evfd_rq = eventfd(0, 0);
    evfd_done = eventfd(0, 0);
    assert(evfd_rq != -1 && evfd_done != -1);

    positional_fd = openIndex(positional_filename, 0x50584449);
    lemmatized_fd = openIndex(lemmatized_filename, 0x4c584449);
    fd = -1;
    rqs_left = 0;

    assert(pthread_create(&thread, NULL, &runThreadFunc, this) == 0);

    info("postings source running\n");
}

void PostingsSource::stop()
{
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    close(evfd_rq);
    close(evfd_done);
    talloc_free(memctx);
}

void PostingsSource::request(ReadRq *rqs, int count, IndexType idxtype)
{
    info("requesting %d posting lists from %s index for terms:",
            count, idxtype == LEMMATIZED ? "lemmatized" : "positional");
    for(int i=0; i<count; i++)
        info(" 0x%08x", rqs[i].term_id);
    info("\n");

    if(!count)
        return;

    fd = idxtype == POSITIONAL ? positional_fd : lemmatized_fd;
    this->rqs = rqs;
    rqs_count = rqs_left = count;

    eventfd_write(evfd_rq, 1);
}

int PostingsSource::wait(void)
{
    if(!rqs_left)
        return 0;

    uint64_t val;
    eventfd_read(evfd_done, &val);
    rqs_left -= val;
    info("read %d posting lists\n", (int)val);
    return val;
}

static PostingsSource posrc; /* GLOBAL */

/* -------------------------------------------------------------------------- */

class PostingsDecoder
{
public:
    static inline void *decode_lemmatized(Reader rd, int count);
    static inline void *decode_positional(Reader rd, int count);
};

void* PostingsDecoder::decode_lemmatized(Reader rd, int count)
{
    assert(count > 0);
    int *ret = talloc_array(NULL, int, count);

    ret[0] = rd.read_u24();
    for(int i=1; i<count; i++)
        ret[i] = ret[i-1] + rd.read_uv();

    return ret;
}


void* PostingsDecoder::decode_positional(Reader rd, int count)
{
    /* TODO */
    abort();
}

/* -------------------------------------------------------------------------- */

struct QueryNode
{
    enum Type { AND, OR, PHRASE, TERM } type;

    QueryNode *lhs, *rhs, *parent;

    const char *term_text; /* actual term text */
    Dictionary::Key term_id; /* index of term in dictionary */

    int estim_postings; /* estimate number of entries in posting list */
    int depth; /* depth of sub-tree */

    int n_postings; /* number of entries in posting list */
    void *postings; /* actual posting list */
};

/* -------------------------------------------------------------------------- */

class QueryParser
{
    void *memctx;
    TermReader trd;

    QueryNode *makeNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs, const char *term) const;

    QueryNode *parse0(); /* parentheses + terms */;
    QueryNode *parse1(); /* or-s */
    QueryNode *parse2(); /* and-s */
    QueryNode *parsePhrase(); /* "query" */
    QueryNode *parseQuery();

    QueryNode *do_run(const char *query);

public:
    static inline QueryNode *run(const char *query);
};

QueryNode *QueryParser::makeNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs, const char *term) const
{
    QueryNode *node = talloc_zero(memctx, QueryNode);
    assert(node);
    node->type = type;
    node->rhs = rhs; if(rhs) rhs->parent = node;
    node->lhs = lhs; if(lhs) lhs->parent = node;
    node->parent = NULL;
    node->term_text = term;
    return node;
}

QueryNode *QueryParser::parse0() /* parentheses + terms */
{
    if(trd.eatSymbol('(')) {
        QueryNode *inside = parse2();
        if(!trd.eatSymbol(')'))
            throw "unmatched parentheses";
        return inside;
    }
    char *term = trd.readTerm();
    if(!term) return NULL;

    talloc_steal(memctx, term);
    return makeNode(QueryNode::TERM, NULL, NULL, term);
}

QueryNode *QueryParser::parse1() /* or-s */
{
    QueryNode *lhs = parse0();
    if(!trd.eatSymbol('|'))
        return lhs;
    QueryNode *rhs = parse1();
    if(!rhs)
        throw "no rhs for '|'";
    return makeNode(QueryNode::OR, lhs, rhs, NULL);
}

QueryNode *QueryParser::parse2() /* and-s */
{
    QueryNode *lhs = parse1();
    if(!trd.eatWhitespace())
        return lhs;
    QueryNode *rhs = parse2();
    if(!rhs)
        return lhs;
    return makeNode(QueryNode::AND, lhs, rhs, NULL);
}

QueryNode *QueryParser::parsePhrase() /* "query" */
{
    if(!trd.eatSymbol('"'))
        return NULL;

    QueryNode *root = makeNode(QueryNode::PHRASE, NULL, NULL, NULL);

    QueryNode *last = root;
    while(!trd.eof())
    {
        char *tstr = trd.readTerm();
        if(!tstr) break;
        talloc_steal(tstr, memctx);

        QueryNode *term = makeNode(QueryNode::TERM, NULL, NULL, tstr);
        talloc_steal(last, term);

        last->rhs = term;
        last = term;
    }

    if(!trd.eatSymbol('"'))
        throw "unmatched quotes";

    return root;
}

QueryNode *QueryParser::parseQuery()
{
    QueryNode *root = NULL;
    if(!root) root = parsePhrase();
    if(!root) root = parse2();
    trd.eatWhitespace();

    if(trd.eof()) return root;
    throw "garbage at end";
}

QueryNode *QueryParser::do_run(const char *query)
{
    memctx = talloc_new(NULL);
    trd.attach(query, strlen(query));
    trd.setQueryReadingMode();

    try {
        return parseQuery();
    } catch(const char *err) {
        printf("malformed query: %s\n", err);
    }

    talloc_free(memctx);
    return NULL;
}

QueryNode *QueryParser::run(const char *query) {
    QueryParser qp;
    return qp.do_run(query);
}

/* -------------------------------------------------------------------------- */

static void resolve_terms(QueryNode *node, IndexType idxtype)
{
    if(!node) return;
    if(node->type == QueryNode::TERM)
    {
        node->term_id =
            dictionary.lookup(node->term_text, strlen(node->term_text), idxtype);

        if(node->term_id != -1) {
            Dictionary::PostingListInfo info = 
                dictionary.getPostingListInfo(node->term_id);
            node->n_postings = info.n_postings;

            info("resolved term »%s« (0x%08x): has %d postings, list at 0x%08llx, size %zu\n",
                  node->term_text, node->term_id, node->n_postings, (unsigned long long)info.offset, info.size);
        }

        if(node->n_postings == 0)
            node->postings = &empty_posting_list;
    } else {
        resolve_terms(node->lhs, idxtype);
        resolve_terms(node->rhs, idxtype);
    }
}

/* -------------------------------------------------------------------------- */

class BooleanQueryOptimizer
{
    QueryNode **scratchpad;

    static int count_nodes(const QueryNode *node);
    static void linearize(QueryNode *node);
    QueryNode *arrange(QueryNode *node);

    static void fix_parents(QueryNode *node);
    static void check_parents(const QueryNode *node);

    QueryNode *do_run(QueryNode *root);

public:
    static inline QueryNode *run(QueryNode *root);
};

int BooleanQueryOptimizer::count_nodes(const QueryNode *node)
{
    return !node ? 0 : 1 + count_nodes(node->lhs) + count_nodes(node->rhs);
}

void BooleanQueryOptimizer::linearize(QueryNode *node)
{
    if(!node) return;
     
    while(node->lhs && node->lhs->type == node->type &&
          node->rhs && node->rhs->type == node->type)
    {
        QueryNode *p = node->lhs, *q = node->rhs;
        node->rhs = q->rhs;
        q->rhs = q->lhs;
        q->lhs = p;
        node->lhs = q;
    }

    if(node->lhs && node->lhs->type != node->type &&
       node->rhs && node->rhs->type == node->type)
        std::swap(node->lhs, node->rhs);

    linearize(node->lhs);
    linearize(node->rhs);
}

QueryNode *BooleanQueryOptimizer::arrange(QueryNode *node)
{
    /* check node type */
    QueryNode::Type type = node->type;
    if(type == QueryNode::TERM) {
        node->estim_postings = node->n_postings;
        return node;
    }

    /* extract nodes of this group (sharing the functor */
    QueryNode **base = scratchpad, **pool = base;
    for(QueryNode *p = node; p->type == type; p = p->lhs)
        *scratchpad++ = p;

    /* extract children of this group's nodes */   
    QueryNode **children = scratchpad;
    for(QueryNode **p = pool; p < children; p++)
        *scratchpad++ = (*p)->rhs = arrange((*p)->rhs);
    *scratchpad++ = children[-1]->lhs = arrange(children[-1]->lhs);

    QueryNode **end = scratchpad;

    info("node %p: root of group type %s, size: %d, children: %d\n",
         node, type == QueryNode::AND ? "AND" : "OR",
         (int)(children-base), (int)(end - children));

    /* check children for empty nodes
     * for AND, if one is found, make entire group as an empty
     * node; for OR just remove them. */
    if(type == QueryNode::AND) {
        for(QueryNode **p = children; p<end; p++) 
            if((*p)->estim_postings == 0) {
                info("found empty node %p in AND group, emptying whole group\n", (*p));
                node->type = QueryNode::TERM;
                node->n_postings = node->estim_postings = 0;
                node->postings = &empty_posting_list;
                scratchpad = base;
                return node;
            }
    } else {
        for(QueryNode **p = children; p<end; ) 
            if((*p)->estim_postings == 0) {
                info("found empty node %p in OR group, removing\n", (*p));
                *p = *--end;
            } else
                p++;
    }

    /* remove duplicate terms */
    for(QueryNode **p = children; p<end; )
    {
        if((*p)->type != QueryNode::TERM)
            { p++; continue; }

        bool dupli = false;
        for(QueryNode **q = children; q<p; q++)
            if((*q)->type == (*p)->type && (*q)->term_id == (*p)->term_id) {
                info("node %p is duplicate of node %p, removing\n", (*p), (*q));
                dupli = true;
                break;
            }

        if(dupli)
            *p = *--end;
        else
            p++;
    }

    /* find optimal execution order */
    while(end - children >= 2)
    {
        /* end[-1] is to be the child with smallest estim_postings and
           end[-2] is to be the child with second-smalest estim_postings */
        if(end[-1]->estim_postings > end[-2]->estim_postings)
            std::swap(end[-1], end[-2]);

        for(QueryNode **p = children; p<end-2; p++)
            if((*p)->estim_postings < end[-1]->estim_postings) {
                std::swap(end[-1], end[-2]);
                std::swap(*p, end[-1]);
            }
            else if((*p)->estim_postings < end[-2]->estim_postings)
                std::swap(*p, end[-2]);

        /* set up the new node (reusing one of the old ones)
           the deeper sub-tree always goes to the lhs. */
        QueryNode *v = *pool++;
        v->lhs = end[-1];
        v->rhs = end[-2];
        
        if(v->lhs->depth < v->rhs->depth)
            std::swap(v->lhs, v->rhs);
        v->depth = v->lhs->depth + 1;

        v->estim_postings =
            type == QueryNode::OR
                ? v->lhs->estim_postings + v->rhs->estim_postings
                : std::min(v->lhs->estim_postings, v->rhs->estim_postings);

        /* insert the new node in place of the old ones */
        end[-2] = v;
        end--;
    }

    scratchpad = base;
    return children[0];
}

void BooleanQueryOptimizer::fix_parents(QueryNode *node)
{
    if(!node) return;
    if(node->lhs) node->lhs->parent = node;
    if(node->rhs) node->rhs->parent = node;
    fix_parents(node->lhs);
    fix_parents(node->rhs);
}

void BooleanQueryOptimizer::check_parents(const QueryNode *node)
{
    if(!node) return;
    assert(!node->lhs || node->lhs->parent == node);
    assert(!node->rhs || node->rhs->parent == node);
    check_parents(node->lhs);
    check_parents(node->rhs);
}

QueryNode *BooleanQueryOptimizer::do_run(QueryNode *root)
{
    QueryNode *canary = (QueryNode *)0xbadc0de1l;

    int node_count = count_nodes(root);
    QueryNode *_scratchpad[node_count+1];
    
    scratchpad = _scratchpad;
    _scratchpad[node_count] = canary;
    
    linearize(root);
    root = arrange(root);
    fix_parents(root);
    check_parents(root);

    assert(scratchpad == _scratchpad &&
           _scratchpad[node_count] == canary);

    return root;
}

QueryNode *BooleanQueryOptimizer::run(QueryNode *root) {
    BooleanQueryOptimizer opt;
    return opt.do_run(root);
}



class BooleanQueryEngine
{
    void *memctx;

    static int countTerms(const QueryNode *node);
    static int extractTerms(QueryNode *node, QueryNode **ptr);

    void evaluateAndNode(QueryNode *node) __attribute__((hot));
    void evaluateOrNode(QueryNode *node) __attribute__((hot));
    inline void evaluateNode(QueryNode *node);

    void do_run(QueryNode *root);

public:
    static inline void run(QueryNode *root);
};

int BooleanQueryEngine::countTerms(const QueryNode *node)
{
    return !node ? 0 :
           node->type == QueryNode::TERM ? 1 :
           countTerms(node->lhs) + countTerms(node->rhs);
}

int BooleanQueryEngine::extractTerms(QueryNode *node, QueryNode **ptr)
{
    if(!node)
        return 0;
    if(node->type == QueryNode::TERM) {
        *ptr = node;
        return 1;
    }
    int l = extractTerms(node->lhs, ptr);
    int r = extractTerms(node->rhs, ptr+l);
    return l+r;
}

void BooleanQueryEngine::evaluateNode(QueryNode *node)
{
    if(!node || node->postings ||
       !node->lhs->postings || !node->rhs->postings)
        return;

    info("evaluating node %p: ", node);

    if(node->type == QueryNode::AND)
        evaluateAndNode(node);
    else if(node->type == QueryNode::OR)
        evaluateOrNode(node);
    else
        abort();

    info("got %d postings\n", node->n_postings);

    evaluateNode(node->parent);
}

void BooleanQueryEngine::evaluateAndNode(QueryNode *node)
{
    if(node->lhs->n_postings == 0 ||
       node->rhs->n_postings == 0) {
        node->postings = &empty_posting_list;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->n_postings, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->n_postings, *Bend = B + m;

    int *C = talloc_array(memctx, int, std::min(n,m));
    node->postings = C;
    assert(C);

    while(A < Aend && B < Bend)
        if(*A < *B)
            A++;
        else if(*A > *B)
            B++;
        else 
            *C++ = *A, A++, B++;

    node->n_postings = (C - (int *)node->postings);
}

void BooleanQueryEngine::evaluateOrNode(QueryNode *node)
{
    if(node->lhs->n_postings == 0) {
        node->postings = node->rhs->postings;
        node->n_postings = node->rhs->n_postings;
        return;
    }
    if(node->rhs->n_postings == 0) {
        node->postings = node->lhs->postings;
        node->n_postings = node->lhs->n_postings;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->n_postings, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->n_postings, *Bend = B + m;

    int *C = talloc_array(memctx, int, n+m);
    node->postings = C;
    assert(C);

    while(A < Aend && B < Bend)
        if(*A < *B)
            *C++ = *A++;
        else if(*A > *B)
            *C++ = *B++;
        else 
            *C++ = *A, A++, B++;
    if(A != Aend)
        memcpy(C, A, (Aend - A) * sizeof(int));
    if(B != Bend)
        memcpy(C, B, (Bend - B) * sizeof(int));

    node->n_postings = (C - (int *)node->postings) + (Aend - A) + (Bend - B);
}

void BooleanQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);

    resolve_terms(root, LEMMATIZED);
    info("raw query:\n"); dump_query_tree(root);
    root = BooleanQueryOptimizer::run(root);
    info("optimized query:\n"); dump_query_tree(root);

    int term_count = countTerms(root);
    QueryNode *terms[term_count];
    extractTerms(root, terms);

    int rqs_count = 0, rqs_done = 0;
    PostingsSource::ReadRq rqs[term_count];
    for(int i=0; i<term_count; i++)
    {
        if(terms[i]->postings)
            continue;

        bool dupli = false;
        for(int j=0; j<i; j++)
            if(terms[j]->term_id == terms[i]->term_id) {
                info("term %d is duplicate of term %d\n", i,j);
                dupli = true;
                break;
            }
        if(dupli) continue;

        rqs[rqs_count++].term_id = terms[i]->term_id;
    }

    posrc.request(rqs, rqs_count, LEMMATIZED);

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + (rqs_done++);
            talloc_steal(memctx, rq->data);

            Dictionary::PostingListInfo info =
                dictionary.getPostingListInfo(rq->term_id);
            void *decoded = PostingsDecoder::decode_lemmatized(
                                Reader(rq->data, info.size), info.n_postings);
            talloc_steal(memctx, decoded);

            info("processing posting list for term 0x%08x\n", rq->term_id);

            for(int i=0; i<term_count; i++) 
                if(terms[i]->term_id == rq->term_id) {
                    terms[i]->postings = decoded;
                    evaluateNode(terms[i]->parent);
                }
        }

    const int *postings = (const int *)root->postings;
    assert(postings);
    printf("--- RESULTS: %d pages\n", root->n_postings);
    for(int i=0; i<root->n_postings; i++) 
        printf("  %d: %s\n", i+1, artitles.lookup(postings[i]));
}

void BooleanQueryEngine::run(QueryNode *root) {
    BooleanQueryEngine e;
    e.do_run(root);
}

/* -------------------------------------------------------------------------- */

class PhraseQueryEngine
{
    void *memctx;

public:
    static void run(QueryNode *root);
};

void PhraseQueryEngine::run(QueryNode *root)
{
    ; /* TODO */
}

/* -------------------------------------------------------------------------- */

static void dump_query_tree(const QueryNode *node, int indent)
{
    static const char spaces[] = "                                                                                ";
    info("%.*s %p: ", indent, spaces, node);
    if(!node) {
        info("NULL\n");
        return;
    }
    switch(node->type) {
        case QueryNode::TERM:
            info("TERM: »%s« (0x%08x), postings: %d\n",
                node->term_text, node->term_id, node->n_postings);
            return;
        case QueryNode::OR:
            info("OR, expected: %d\n", node->estim_postings);
            dump_query_tree(node->lhs, 3+indent);
            dump_query_tree(node->rhs, 3+indent);
            return;
        case QueryNode::AND:
            info("AND, expected: %d\n", node->estim_postings);
            dump_query_tree(node->lhs, 3+indent);
            dump_query_tree(node->rhs, 3+indent);
            return;
        case QueryNode::PHRASE:
            info("PHRASE\n");
            for(QueryNode *p = node->rhs; p; p = p->rhs)
                dump_query_tree(p, 3+indent);
            return;
    }
}

static void run_query(const char *query)
{
    Timer timer; timer.start();

    QueryNode *root = QueryParser::run(query);
    if(!root)
        return;

    if(root->type == QueryNode::PHRASE)
        PhraseQueryEngine::run(root);
    else
        BooleanQueryEngine::run(root);

    talloc_free(talloc_parent(root));

    printf("--- total time: %.3lf seconds\n", timer.end());
}

int main(void)
{
    artitles.read("db/artitles");
    dictionary.read("db/dictionary");
    posrc.start("db/positional", "db/lemmatized");

    for(;;) {
        static char buffer[1024];
        printf("Enter query: "); fflush(stdout);
        if(fgets(buffer, 1023, stdin) == NULL)
            break;
        run_query(buffer);
    }

    return 0;
}
