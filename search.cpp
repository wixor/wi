#include <math.h>
#include <time.h>
#include <getopt.h>
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

static char queryText[1024];
static bool verbose, noResults;
#define info(fmt, ...) \
    do { if(unlikely(verbose)) printf(fmt, ## __VA_ARGS__); } while (0)

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
    int rc = clock_gettime(clock, &ts);
    assert(rc == 0); (void)rc;
}
double Timer::end()
{
    struct timespec now;
    int rc = clock_gettime(clock, &now);
    assert(rc == 0); (void)rc;
    return 1e-9*(now.tv_nsec - ts.tv_nsec) +
                (now.tv_sec  - ts.tv_sec);
}

/* -------------------------------------------------------------------------- */

class Artitles
{
    void *memctx;
    int n_articles;
    float *pageranks;
    uint16_t *term_counts;
    char **titles;

public:
    Artitles();
    ~Artitles();
    void read(const char *filename);
    inline int size() const { return n_articles; }
    inline const char *getTitle(int key) const { return titles[key]; }
    inline int getTermCount(int key) const { return term_counts[key]; }
    inline float getPageRank(int key) const { return pageranks[key]; }
};

Artitles::Artitles() {
    memctx = NULL;
}
Artitles::~Artitles() {
    if(memctx) talloc_free(memctx);
}

static Artitles artitles; /* GLOBAL */

void Artitles::read(const char *filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "article titles");
    assert(memctx);

    info("reading article titles...\n");

    Timer timer; timer.start();

    FileIO fio(filename, O_RDONLY);
    
    { uint32_t hdr[2];
      fio.read_raw(hdr, sizeof(hdr));
      assert(hdr[0] == 0x4c544954);
      n_articles = hdr[1];
    }

    pageranks = (float *)fio.read_raw_alloc(sizeof(float)*n_articles);
    term_counts = (uint16_t *)fio.read_raw_alloc(sizeof(uint16_t)*n_articles);
    off_t offs = fio.tell(), end = fio.seek(0, SEEK_END);
    char *title_texts = (char *)fio.read_raw_alloc(end-offs, offs),
         *title_texts_end = title_texts + (end-offs);
    talloc_steal(memctx, pageranks);
    talloc_steal(memctx, term_counts);
    talloc_steal(memctx, title_texts);

    fio.close();

    titles = talloc_array(memctx, char *, n_articles);
    assert(titles);

    for(int i=0; i<n_articles; i++) {
        titles[i] = title_texts;
        title_texts =
            (char *)memchr(title_texts, '\0', title_texts_end - title_texts);
        assert(title_texts);
        title_texts++;
    }

    info("article titles read in %.03lf seconds.\n", timer.end());
}

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
        char *text; /* the actual word */
        short text_length; /* how long is the word (bytes) */
        bool stop; /* if the term is a stopword */
    };

    void *memctx;

    int term_count;
    TermHasher hasher;
    PostingList *lemmatized, *positional;
    Term *terms, **buckets;

    void do_read(Reader rd);

public:
    struct PostingsInfo {
        off_t offset;
        size_t size;
        int n_postings;
    };

    Dictionary();
    ~Dictionary();

    void read(const char *filename);
    
    class Key {
        int key;
    public:
        inline Key() : key(0) { }
        inline Key(int key) : key(key) { }
        inline IndexType getIndexType() const;
        inline PostingsInfo getPostingsInfo() const;
        inline off_t getOffset() const { return getPostingsInfo().offset; }
        inline size_t getSize() const { return getPostingsInfo().size; }
        inline int getNPostings() const { return getPostingsInfo().n_postings; }
        inline operator int() const { return key; }
        inline bool operator==(Key k) { return key == k.key; }
    };
   
    inline int size() const { return term_count; }
    int lookup(const char *text, size_t len) const;
    inline bool isStopWord(int term_id) const;
    inline float getIDF(int term_id) const;
    inline Key getPostingsKey(int term_id, IndexType idxtype) const;
};

static Dictionary dictionary; /* GLOBAL */

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
    rd.assert_u32(0x54434944);

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
    term_count = 0;
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
                if(t->text_length & 0x80) {
                    t->stop = true;
                    t->text_length &=~ 0x80;
                }
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

int Dictionary::lookup(const char *text, size_t len) const
{
    int h = hasher.hash(text, len);

    const Term *t = buckets[h], *tend = buckets[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)t->text_length)
            continue;
        if(memcmp(text, t->text, len) != 0)
            continue;
    
        return buckets[h] - buckets[0] + i;
    }

    return -1;
}

bool Dictionary::isStopWord(int term_id) const {
    return terms[term_id].stop;
}
float Dictionary::getIDF(int term_id) const {
    int k = lemmatized[terms[term_id].lemmatized_list_id].n_postings;
    return logf((float)artitles.size() / (float)k);
}

Dictionary::Key Dictionary::getPostingsKey(int term_id, IndexType idxtype) const
{
    switch(idxtype) {
        case LEMMATIZED: return Key(terms[term_id].lemmatized_list_id | 0x80000000);
        case POSITIONAL: return Key(term_id);
    }
    abort();
}

IndexType Dictionary::Key::getIndexType() const {
    return key & 0x80000000 ? LEMMATIZED : POSITIONAL;
}

Dictionary::PostingsInfo Dictionary::Key::getPostingsInfo() const
{
    int k = key & ~0x80000000;
    PostingList *lists =
        (key & 0x80000000) ? dictionary.lemmatized : dictionary.positional;

    PostingsInfo ret;
    ret.offset = lists[k].offset;
    ret.size = lists[k+1].offset - lists[k].offset;
    ret.n_postings = lists[k].n_postings;

    return ret;
}

/* -------------------------------------------------------------------------- */

class PostingsSource
{
public:
    struct ReadRq {
        Dictionary::Key postings_key;
        void *data;
    };

private:
    void *memctx;
    int evfd_rq, evfd_done;
    int lemmatized_fd, positional_fd;

    pthread_t thread;
    ReadRq *rqs;
    int rqs_count, rqs_left;

    static int openIndex(const char *filename, uint32_t magic);
    static void* runThreadFunc(void *);
    void threadFunc();

public:
    PostingsSource();
    ~PostingsSource();

    void start(const char *positional_filename, const char *lemmatized_filename);
    void stop();

    void request(ReadRq *rqs, int count);
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

        int rqs_count = *(volatile int *)&this->rqs_count;
        PostingsSource::ReadRq *rqs = *(PostingsSource::ReadRq * volatile *)&this->rqs;

        for(int i=0; i<rqs_count; i++)
        {
            IndexType idxtype = 
                rqs[i].postings_key.getIndexType();
            Dictionary::PostingsInfo info =
                rqs[i].postings_key.getPostingsInfo();
            
            int fd = idxtype == LEMMATIZED ? lemmatized_fd : positional_fd;
            FileIO(fd).read_raw(rqs[i].data, info.size, info.offset);

            eventfd_write(evfd_done, 1);
        }
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
    rqs_left = 0;

    int rc = pthread_create(&thread, NULL, &runThreadFunc, this);
    assert(rc == 0); (void)rc;

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

void PostingsSource::request(ReadRq *rqs, int count)
{
    info("requesting %d posting lists:", count);
    for(int i=0; i<count; i++)
        info(" 0x%08x", (int)rqs[i].postings_key);
    info("\n");

    if(!count)
        return;

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

struct QueryNode
{
    enum Type { AND, OR, PHRASE, TERM } type;

    QueryNode *lhs, *rhs, *parent;

    const char *term_text; /* actual term text */
    int term_id; /* index of term in dictionary */
    Dictionary::Key postings_key; /* postings list identifier */

    int estim_postings; /* estimate number of entries in posting list */
    int depth; /* depth of sub-tree */

    int n_postings; /* number of entries in posting list */
    void *postings; /* actual posting list */
};

static long empty_posting_list = -42; /* GLOBAL */

static QueryNode makeEmptyQueryNode()
{
    QueryNode node;
    node.type = QueryNode::TERM;
    node.lhs = node.rhs = node.parent = NULL;
    node.term_text = NULL;
    node.term_id = -1;
    node.depth = node.estim_postings = node.n_postings = 0;
    node.postings = &empty_posting_list;
    return node;
}
static const QueryNode emptyQueryNode = makeEmptyQueryNode(); /* GLOBAL */

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
    static inline QueryNode *run(const char *query) {
        return QueryParser().do_run(query);
    }
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

    try {
        return parseQuery();
    } catch(const char *err) {
        printf("malformed query: %s\n", err);
    }

    talloc_free(memctx);
    return NULL;
}

/* -------------------------------------------------------------------------- */

class QueryEngineBase
{
protected:
    void *memctx;
    QueryNode **terms;
    PostingsSource::ReadRq *rqs;
    int term_count, rqs_count;

    static inline void dumpQueryTree(const QueryNode *node, const char *title);
    static void dumpQueryTree(const QueryNode *node, int indent);

    static void resolveTerms(QueryNode *node, IndexType idxtype);
    static int countTerms(const QueryNode *node);
    static int countNodes(const QueryNode *node);
    static int extractTerms(QueryNode *node, QueryNode **ptr);
    void createRqs();
};

void QueryEngineBase::dumpQueryTree(const QueryNode *node, const char *title)
{
    if(!verbose) return;
    puts(title);
    dumpQueryTree(node, 3);
}

void QueryEngineBase::dumpQueryTree(const QueryNode *node, int indent)
{
    static const char spaces[] = "                                                                                ";
    printf("%.*s %p: ", indent, spaces, node);
    if(!node) {
        printf("NULL\n");
        return;
    }
    switch(node->type) {
        case QueryNode::TERM:
            printf("TERM: »%s« (%d), postings: %d\n",
                node->term_text, node->term_id, node->n_postings);
            return;
        case QueryNode::OR:
            printf("OR, expected: %d\n", node->estim_postings);
            dumpQueryTree(node->lhs, 3+indent);
            dumpQueryTree(node->rhs, 3+indent);
            return;
        case QueryNode::AND:
            printf("AND, expected: %d\n", node->estim_postings);
            dumpQueryTree(node->lhs, 3+indent);
            dumpQueryTree(node->rhs, 3+indent);
            return;
        case QueryNode::PHRASE:
            printf("PHRASE\n");
            for(const QueryNode *p = node->rhs; p; p = p->rhs)
                dumpQueryTree(p, 3+indent);
            return;
    }
}

void QueryEngineBase::resolveTerms(QueryNode *node, IndexType idxtype)
{
    if(!node) return;
    if(node->type == QueryNode::TERM)
    {
        node->term_id =
            dictionary.lookup(node->term_text, strlen(node->term_text));

        if(node->term_id == -1) 
            info("term »%s« does not appear in dictionary\n",
                 node->term_text);
        else {
            node->postings_key =
                dictionary.getPostingsKey(node->term_id, idxtype);
            Dictionary::PostingsInfo info =
                node->postings_key.getPostingsInfo();
            node->n_postings = info.n_postings;

            info("resolved term »%s« (%d): postings id 0x%08x, has %d postings at 0x%08llx, size %zu\n",
                  node->term_text, node->term_id, (int)node->postings_key, node->n_postings, (unsigned long long)info.offset, info.size);
        }

        if(node->n_postings == 0)
            node->postings = &empty_posting_list;
    } 
    resolveTerms(node->lhs, idxtype);
    resolveTerms(node->rhs, idxtype);
}

int QueryEngineBase::countTerms(const QueryNode *node) {
    if(node)
        return (node->type == QueryNode::TERM) +
               countTerms(node->lhs) + countTerms(node->rhs);
    else
        return 0;
}

int QueryEngineBase::countNodes(const QueryNode *node) {
    return !node ? 0 : 1 + countNodes(node->lhs) + countNodes(node->rhs);
}

int QueryEngineBase::extractTerms(QueryNode *node, QueryNode **ptr)
{
    if(!node)
        return 0;
    if(node->type == QueryNode::TERM)
        *ptr++ = node;
    int l = extractTerms(node->lhs, ptr);
    int r = extractTerms(node->rhs, ptr+l);
    return (node->type == QueryNode::TERM)+l+r;
}

void QueryEngineBase::createRqs()
{
    rqs_count = 0;
    for(int i=0; i<term_count; i++)
    {
        if(terms[i]->postings)
            continue;

        bool dupli = false;
        for(int j=0; j<i; j++)
            if(terms[j]->postings_key == terms[i]->postings_key) {
                info("term %d is duplicate of term %d\n", i,j);
                dupli = true;
                break;
            }
        if(dupli) continue;

        PostingsSource::ReadRq *rq = rqs + (rqs_count++);
        rq->postings_key = terms[i]->postings_key;
        rq->data = talloc_size(memctx, rq->postings_key.getSize());
        assert(rq->data);
    }
}

/* -------------------------------------------------------------------------- */

class BooleanQueryEngine : public QueryEngineBase
{
    QueryNode **scratchpad;
    int node_count, stopword_count;

    static int countStopwords(const QueryNode *node);

    static void linearize(QueryNode *node);
    QueryNode *optimize(QueryNode *node);
    static void fixParents(QueryNode *node);

    inline void processPostings();
    inline int* decodePostings(Reader rd, int count);
    inline void evaluateNode(QueryNode *node);
    void evaluateAndNode(QueryNode *node) __attribute__((hot));
    void evaluateOrNode(QueryNode *node) __attribute__((hot));
    inline void printResult(const QueryNode *root);

    void do_run(QueryNode *root);

public:
    static inline void run(QueryNode *root) {
        BooleanQueryEngine().do_run(root);
    }
};

int BooleanQueryEngine::countStopwords(const QueryNode *node) {
    if(node->type == QueryNode::TERM)
        return dictionary.isStopWord(node->term_id);
    if(node->type == QueryNode::AND)
        return countStopwords(node->lhs) + countStopwords(node->rhs);
    return 0;
}

void BooleanQueryEngine::linearize(QueryNode *node)
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

QueryNode *BooleanQueryEngine::optimize(QueryNode *node)
{
    /* check node type */
    QueryNode::Type type = node->type;
    if(type == QueryNode::TERM) {
        node->estim_postings = node->n_postings;
        return node;
    }

    /* extract nodes of this group (sharing the functor) */
    QueryNode **base = scratchpad, **pool = base;
    for(QueryNode *p = node; p->type == type; p = p->lhs)
        *scratchpad++ = p;

    /* extract children of this group's nodes */   
    QueryNode **children = scratchpad;
    for(QueryNode **p = pool; p < children; p++)
        *scratchpad++ = (*p)->rhs = optimize((*p)->rhs);
    *scratchpad++ = children[-1]->lhs = optimize(children[-1]->lhs);

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
                *node = emptyQueryNode;
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
            if((*q)->type == (*p)->type && (*q)->postings_key == (*p)->postings_key) {
                info("node %p is duplicate of node %p, removing\n", (*p), (*q));
                dupli = true;
                break;
            }

        if(dupli)
            *p = *--end;
        else
            p++;
    }

    /* remove stopwords */
    if(type == QueryNode::AND && stopword_count*2 < term_count) 
        for(QueryNode **p = children; p<end; )
            if((*p)->type != QueryNode::TERM ||
               !dictionary.isStopWord((*p)->term_id))
                p++;
            else 
                *p = *--end;

    /* check if there are any nodes left */
    if(children == end) {
        *node = emptyQueryNode;
        scratchpad = base;
        return node;
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

void BooleanQueryEngine::fixParents(QueryNode *node)
{
    if(!node) return;
    if(node->lhs) node->lhs->parent = node;
    if(node->rhs) node->rhs->parent = node;
    fixParents(node->lhs);
    fixParents(node->rhs);
}

void BooleanQueryEngine::processPostings()
{
    int rqs_done = 0;

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + (rqs_done++);

            Dictionary::PostingsInfo info =
                rq->postings_key.getPostingsInfo();
            int *decoded =
                decodePostings(Reader(rq->data, info.size), info.n_postings);

            info("processing posting list 0x%08x\n", (int)rq->postings_key);

            for(int i=0; i<term_count; i++) 
                if(terms[i]->postings_key == rq->postings_key) {
                    terms[i]->postings = decoded;
                    evaluateNode(terms[i]->parent);
                }
        }
}

int* BooleanQueryEngine::decodePostings(Reader rd, int count)
{
    assert(count > 0);
    int *ret = talloc_array(memctx, int, count);
    assert(ret);

    ret[0] = rd.read_u24(); /* document id */
    rd.read_uv(); /* term-frequency */
    for(int i=1; i<count; i++) {
        ret[i] = ret[i-1] + rd.read_uv(); /* document id */
        rd.read_uv(); /* term-frequency */
    }

    return ret;
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

void BooleanQueryEngine::printResult(const QueryNode *root)
{
    const int *postings = (const int *)root->postings;
    assert(postings);

    printf("QUERY: %s TOTAL: %d\n", queryText, root->n_postings);
    if(!noResults)
        for(int i=0; i<root->n_postings; i++) 
            puts(artitles.getTitle(postings[i]));
}

void BooleanQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);

    resolveTerms(root, LEMMATIZED);
    
    dumpQueryTree(root, "raw query:");

    node_count = countNodes(root);
    stopword_count = countStopwords(root);

    QueryNode *_scratchpad[node_count]; scratchpad = _scratchpad;

    term_count = countTerms(root);
    linearize(root);
    root = optimize(root);
    fixParents(root);

    dumpQueryTree(root, "optimized query:");
    
    term_count = countTerms(root);
    QueryNode *_terms[term_count]; terms = _terms;
    PostingsSource::ReadRq _rqs[term_count]; rqs = _rqs;

    extractTerms(root, terms);
    createRqs();
    posrc.request(rqs, rqs_count);
    processPostings();
    printResult(root);
}


/* -------------------------------------------------------------------------- */

class PhraseQueryEngine : public QueryEngineBase
{
    struct doc {
        int doc_id, positions_offs;
    };

    typedef unsigned short pos_t;
    
    int *offsets;
    struct doc *docs, *docs_rdptr, *docs_wrptr, *docs_end;
    pos_t *positions, *positions_wrptr;

    inline void sortTerms();
    inline void processPostings();
    void processTerm(Reader rd, int n_postings, int offset) __attribute__((hot));
    inline void processDocument(int doc_id, Reader prd, int offset) __attribute__((hot));
    inline void makeWorkingSet(Reader rd, int n_postings, int offset) __attribute__((hot));
    inline void printResult();
    
    void do_run(QueryNode *root);

public:
    static inline void run(QueryNode *root) {
        PhraseQueryEngine().do_run(root);
    }
};

void PhraseQueryEngine::sortTerms()
{
    for(int i=0; i<term_count; i++)
        offsets[i] = term_count-1-i;

    for(int i=0; i<term_count; i++)
        for(int j=i; j > 0 && terms[j]->n_postings < terms[j-1]->n_postings; j--) {
            std::swap(terms[j], terms[j-1]);
            std::swap(offsets[j], offsets[j-1]);
        }
}

void PhraseQueryEngine::processPostings()
{
    int rqs_done = 0;

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + (rqs_done++);

            Dictionary::PostingsInfo info =
                rq->postings_key.getPostingsInfo();
            Reader rd(rq->data, info.size);

            info("processing posting list 0x%08x\n", (int)rq->postings_key);

            for(int i=0; i<term_count; i++) 
                if(terms[i]->postings_key == rq->postings_key)
                    processTerm(rd, info.n_postings, offsets[i]);
        }
}

void PhraseQueryEngine::processTerm(Reader rd, int n_postings, int offset)
{
    /* check if we have something to merge with */
    if(unlikely(!docs)) {
        makeWorkingSet(rd, n_postings, offset);
        return;
    }

    /* but the figers at start */
    docs_rdptr = docs_wrptr = docs;
    positions_wrptr = positions;

    /* where the positions data in bytestream start */
    int positions_offset = 4 + rd.read_u32();

    /* read the first document info */
    int doc_id = rd.read_u24(),
        positions_size = rd.read_uv();

    /* while there are documents left both in bytestream and in our working set */
    while(n_postings-- && docs_rdptr < docs_end)
    {
        /* create the reader for positions and process the document */
        Reader prd((const char *)rd.buffer() + positions_offset, positions_size);
        processDocument(doc_id, prd, offset);

        /* move on to the next document */
        positions_offset += positions_size;
        if(n_postings) {
            doc_id += rd.read_uv();
            positions_size = rd.read_uv();
        }
    }

    /* remember how many documents we have */
    docs_end = docs_wrptr;
}

void PhraseQueryEngine::processDocument(int doc_id, Reader prd, int offset)
{
    /* skip past any documents earlier than this */
    while(docs_rdptr < docs_end && docs_rdptr->doc_id < doc_id)
        docs_rdptr++;

    /* check if our document has been found */
    if(docs_rdptr >= docs_end || docs_rdptr->doc_id > doc_id)
        return;

    /* fetch the document, remember where its positions start */
    struct doc doc = *docs_rdptr;
    const pos_t *positions_rdptr = positions + doc.positions_offs;

    /* move the positions to the current writing finger */
    doc.positions_offs = positions_wrptr - positions;

    /* merge positions. all of them are incremented by 'offset' */
    pos_t p = offset;
    while(!prd.eof() && *positions_rdptr != (pos_t)-1)
    {
        /* get the next position */
        p += prd.read_uv();
        /* skip past position earlier than this */
        while(*positions_rdptr < p) positions_rdptr++;
        /* write the position back, if found */
        if(*positions_rdptr == p)
            *positions_wrptr++ = p;
    }

    /* if any position was written, terminate the positions
     * list with maxval and write the document back */
    if(positions_wrptr - positions > doc.positions_offs) {
        *positions_wrptr++ = (pos_t)-1;
        *docs_wrptr++ = doc;
    }
}


void PhraseQueryEngine::makeWorkingSet(Reader rd, int n_postings, int offset)
{
    /* find out where positions start */
    int positions_offset = 4 + rd.read_u32();

    /* allocate memory (positions may be bigger than necessary) */
    docs = talloc_array(memctx, struct doc, n_postings);
    positions = talloc_array(memctx, pos_t, rd.size() - positions_offset + n_postings);
    assert(docs && positions);

    /* set up writing fingers */
    docs_wrptr = docs;
    positions_wrptr = positions;

    /* read the first document info */
    int doc_id = rd.read_u24(),
        positions_size = rd.read_uv();

    /* while there are documents left both in bytestream and in our buffer */
    while(n_postings--)
    {
        /* create the reader for positions and process the document */
        Reader prd((const char *)rd.buffer() + positions_offset, positions_size);

        /* write the doc */
        docs_wrptr->doc_id = doc_id;
        docs_wrptr->positions_offs = positions_wrptr - positions;
        docs_wrptr++;

        /* write the positions */
        pos_t p = offset;
        while(!prd.eof()) {
            p += prd.read_uv();
            *positions_wrptr++ = p;
        }
        /* terminate the positions list with maxval */
        *positions_wrptr++ = (pos_t)-1;

        /* move on to the next document */
        positions_offset += positions_size;
        if(n_postings) {
            doc_id += rd.read_uv();
            positions_size = rd.read_uv();
        }
    }

    /* remember how many documents we have */
    docs_end = docs_wrptr;
}

void PhraseQueryEngine::printResult()
{
    int n_documents = docs_end - docs;
    printf("QUERY: %s TOTAL: %d\n", queryText, n_documents);
    if(!noResults)
        for(int i=0; i<n_documents; i++) 
            puts(artitles.getTitle(docs[i].doc_id));
}

void PhraseQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);
    docs = docs_rdptr = docs_wrptr = docs_end = NULL;
    positions = positions_wrptr = NULL;

    resolveTerms(root, POSITIONAL);
    
    dumpQueryTree(root, "raw query:");

    term_count = countTerms(root);

    QueryNode *_terms[term_count]; terms = _terms;
    PostingsSource::ReadRq _rqs[term_count]; rqs = _rqs;
    int _offsets[term_count]; offsets = _offsets;

    extractTerms(root, terms);
    sortTerms();

    if(term_count && terms[0]->postings_key.getNPostings()) {
        createRqs();
        posrc.request(rqs, rqs_count);
        processPostings();
    }

    printResult();
}


/* -------------------------------------------------------------------------- */

static void print_usage(void) __attribute__((noreturn));
static void print_usage(void) {
    fprintf(stderr, "usage: search [-v] [-r] [-h]\n"
                    "  -v: print verbose progress information\n"
                    "  -r: do not print title results\n"
                    "  -h: print this help message\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    while(int opt = getopt(argc, argv, "vrh"))
        if(opt == -1) break;
        else switch(opt) {
            case 'v': verbose = true; break;
            case 'r': noResults = true; break;
            case 'h':
            default: print_usage();
        }
    if(optind < argc) print_usage();

    artitles.read("db/artitles");
    dictionary.read("db/dictionary");
    posrc.start("db/positional", "db/lemmatized");

    for(;;)
    {
        if(verbose) printf("Enter query: "), fflush(stdout);
        if(fgets(queryText, sizeof(queryText)-1, stdin) == NULL)
            break;
        {
            char *endl = strchr(queryText, '\n');
            if(endl) *endl = '\0';
        }

        Timer wallTime, cpuTime;
        wallTime.start(CLOCK_MONOTONIC);
        cpuTime.start(CLOCK_PROCESS_CPUTIME_ID);

        QueryNode *root = QueryParser::run(queryText);
        if(!root)
            continue;
        if(root->type == QueryNode::PHRASE)
            PhraseQueryEngine::run(root);
        else
            BooleanQueryEngine::run(root);
        talloc_free(talloc_parent(root));

        info("--- total time: %.3lf ms wall / %.3lf ms cpu\n",
             wallTime.end()*1000.f, cpuTime.end()*1000.f);
    }

    return 0;
}
