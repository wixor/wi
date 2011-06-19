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
static bool verbose, noResults, onlyMarkedDocs;
static int onlyBestDocs;
static struct {
    float alpha, beta, gamma;
} freetextWeights = { 50.f, 1.f, 10.f };

#define info(fmt, ...) \
    do { if(unlikely(verbose)) printf(fmt, ## __VA_ARGS__); } while (0)

template<typename T>
static inline T square(const T &x) { return x*x; }

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
    int articleCount;
    float *tfidfWeights;
    float *pageranks;
    uint16_t *termCounts;
    char **titles;

public:
    Artitles();
    ~Artitles();
    void read(const char *filename);
    inline int size() const { return articleCount; }
    inline const char *getTitle(int key) const { return titles[key]; }
    inline int getTermCount(int key) const { return termCounts[key]; }
    inline float getPageRank(int key) const { return pageranks[key]; }
    inline float getTfIdfWeight(int key) const { return tfidfWeights[key]; }
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
      articleCount = hdr[1];
    }

    tfidfWeights = (float *)fio.read_raw_alloc(sizeof(float)*articleCount);
    pageranks = (float *)fio.read_raw_alloc(sizeof(float)*articleCount);
    termCounts = (uint16_t *)fio.read_raw_alloc(sizeof(uint16_t)*articleCount);
    off_t offs = fio.tell(), end = fio.seek(0, SEEK_END);
    char *titleTexts = (char *)fio.read_raw_alloc(end-offs, offs),
         *titleTextsEnd = titleTexts + (end-offs);
    talloc_steal(memctx, tfidfWeights);
    talloc_steal(memctx, pageranks);
    talloc_steal(memctx, termCounts);
    talloc_steal(memctx, titleTexts);

    fio.close();

    titles = talloc_array(memctx, char *, articleCount);
    assert(titles);

    for(int i=0; i<articleCount; i++) {
        titles[i] = titleTexts;
        titleTexts =
            (char *)memchr(titleTexts, '\0', titleTextsEnd - titleTexts);
        assert(titleTexts);
        titleTexts++;
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
        int postingCount;
    };

    struct Term {
        int lemmatizedListId;
        char *text; /* the actual word */
        short textLength; /* how long is the word (bytes) */
        bool stop; /* if the term is a stopword */
    };

    void *memctx;

    int termCount;
    TermHasher hasher;
    PostingList *lemmatized, *positional;
    Term *terms, **buckets;

    void do_read(Reader rd);

public:
    struct PostingsInfo {
        off_t offset;
        size_t size;
        int postingCount;
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
        inline int getPostingCount() const { return getPostingsInfo().postingCount; }
        inline operator int() const { return key; }
        inline bool operator==(Key k) { return key == k.key; }
    };
   
    inline int size() const { return termCount; }
    int lookup(const char *text, size_t len) const;
    inline bool isStopWord(int termId) const;
    inline Key getPostingsKey(int termId, IndexType idxtype) const;
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

    info("reading dictionary...\n");

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
    int bucketCount = hasher.buckets(),
        lemmatizedListCount = rd.read_u32();

    /* save bucket size reader for future use */
    Reader bucketSizeRd = rd;

    /* count terms */
    termCount = 0;
    for(int i=0; i<bucketCount; i++)
        termCount += rd.read_u8();

    /* allocate stuff */
    lemmatized = talloc_array(memctx, PostingList, lemmatizedListCount+1);
    positional = talloc_array(memctx, PostingList, termCount+1);
    buckets = talloc_array(memctx, Term*, bucketCount+1);
    terms = talloc_array(memctx, Term, termCount);
    assert(lemmatized && positional && terms && buckets);

    { /* read bucket info */
        Term *t = terms;
        for(int i=0; i<bucketCount; i++)
        {
            buckets[i] = t;
            int bucketSize = bucketSizeRd.read_u8();
            while(bucketSize--) {
                t->lemmatizedListId = rd.read_u24();
                t->textLength = rd.read_u8();
                if(t->textLength & 0x80) {
                    t->stop = true;
                    t->textLength &=~ 0x80;
                }
                t++;
            }
        }
        buckets[bucketCount] = t;
    }

    /* read positional list info */
    positional[0].offset = 4;/* because of index file magic number */
    for(int i=1; i<=termCount; i++) {
        positional[i].offset = positional[i-1].offset + rd.read_uv();;
        positional[i-1].postingCount = rd.read_uv();
    }

    /* read lemmatized list info */
    lemmatized[0].offset = 4; /* because of index file magic number */
    for(int i=1; i<=lemmatizedListCount; i++) {
        lemmatized[i].offset = lemmatized[i-1].offset + rd.read_uv();;
        lemmatized[i-1].postingCount = rd.read_uv();
    }

    /* compute total texts length */
    int termTextsLength = 0;
    for(int i=0; i<termCount; i++)
        termTextsLength += terms[i].textLength;

    /* read term texts */
    char *termTexts = (char *)talloc_size(memctx, termTextsLength);
    assert(termTexts);
    rd.read_raw(termTexts, termTextsLength);

    /* bind term texts to terms */
    terms[0].text = termTexts;
    for(int i=1; i<termCount; i++)
        terms[i].text = terms[i-1].text + terms[i-1].textLength;

    /* make sure we read everything */
    assert(rd.eof());
}

int Dictionary::lookup(const char *text, size_t len) const
{
    int h = hasher.hash(text, len);

    const Term *t = buckets[h], *tend = buckets[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)t->textLength)
            continue;
        if(memcmp(text, t->text, len) != 0)
            continue;
    
        return buckets[h] - buckets[0] + i;
    }

    return -1;
}

bool Dictionary::isStopWord(int termId) const {
    return terms[termId].stop;
}

Dictionary::Key Dictionary::getPostingsKey(int termId, IndexType idxtype) const
{
    switch(idxtype) {
        case LEMMATIZED: return Key(terms[termId].lemmatizedListId | 0x80000000);
        case POSITIONAL: return Key(termId);
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
    ret.postingCount = lists[k].postingCount;

    return ret;
}

/* -------------------------------------------------------------------------- */

class PostingsSource
{
public:
    struct ReadRq {
        Dictionary::Key postingsKey;
        void *data;
    };

private:
    void *memctx;
    int evfdRq, evfdDone;
    int lemmatizedFd, positionalFd;

    pthread_t thread;
    ReadRq *rqs;
    int rqCount, rqLeft;

    static int openIndex(const char *filename, uint32_t magic);
    static void* runThreadFunc(void *);
    void threadFunc();

public:
    PostingsSource();
    ~PostingsSource();

    void start(const char *positionalFilename, const char *lemmatizedFilename);
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
        eventfd_read(evfdRq, &foo);

        int rqCount = *(volatile int *)&this->rqCount;
        PostingsSource::ReadRq *rqs = *(PostingsSource::ReadRq * volatile *)&this->rqs;

        for(int i=0; i<rqCount; i++)
        {
            IndexType idxtype = 
                rqs[i].postingsKey.getIndexType();
            Dictionary::PostingsInfo info =
                rqs[i].postingsKey.getPostingsInfo();
            
            int fd = idxtype == LEMMATIZED ? lemmatizedFd : positionalFd;
            FileIO(fd).read_raw(rqs[i].data, info.size, info.offset);

            eventfd_write(evfdDone, 1);
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

void PostingsSource::start(const char *positionalFilename, const char *lemmatizedFilename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "postings reader");
    assert(memctx);
    
    info("starting postings source...\n");

    evfdRq = eventfd(0, 0);
    evfdDone = eventfd(0, 0);
    assert(evfdRq != -1 && evfdDone != -1);

    positionalFd = openIndex(positionalFilename, 0x50584449);
    lemmatizedFd = openIndex(lemmatizedFilename, 0x4c584449);
    rqLeft = 0;

    int rc = pthread_create(&thread, NULL, &runThreadFunc, this);
    assert(rc == 0); (void)rc;

    info("postings source running\n");
}

void PostingsSource::stop()
{
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    close(evfdRq);
    close(evfdDone);
    talloc_free(memctx);
}

void PostingsSource::request(ReadRq *rqs, int count)
{
    info("requesting %d posting lists:", count);
    for(int i=0; i<count; i++)
        info(" 0x%08x", (int)rqs[i].postingsKey);
    info("\n");

    if(!count)
        return;

    this->rqs = rqs;
    rqCount = rqLeft = count;

    eventfd_write(evfdRq, 1);
}

int PostingsSource::wait(void)
{
    if(!rqLeft)
        return 0;

    uint64_t val;
    eventfd_read(evfdDone, &val);
    rqLeft -= val;
    info("read %d posting lists\n", (int)val);
    return val;
}

static PostingsSource posrc; /* GLOBAL */

/* -------------------------------------------------------------------------- */

struct QueryNode
{
    enum Type { AND, OR, PHRASE, FREETEXT, TERM } type;

    QueryNode *lhs, *rhs, *parent;

    const char *termText; /* actual term text */
    int termId; /* index of term in dictionary */
    Dictionary::Key postingsKey; /* postings list identifier */

    int estimPostings; /* estimate number of entries in posting list */
    int depth; /* depth of sub-tree */

    int postingCount; /* number of entries in posting list */
    void *postings; /* actual posting list */
};

static long emptyPostingList = -42; /* GLOBAL */

static QueryNode makeEmptyQueryNode()
{
    QueryNode node;
    node.type = QueryNode::TERM;
    node.lhs = node.rhs = node.parent = NULL;
    node.termText = NULL;
    node.termId = -1;
    node.depth = node.estimPostings = node.postingCount = 0;
    node.postings = &emptyPostingList;
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
    QueryNode *parseFreeText(); /* t1 t2 t3 */
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
    node->termText = term;
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

        last->rhs = term;
        last = term;
    }

    if(!trd.eatSymbol('"'))
        throw "unmatched quotes";

    return root;
}

QueryNode *QueryParser::parseFreeText() /* t1 t2 t3 */
{
    size_t where = trd.tell();
    
    QueryNode *root = makeNode(QueryNode::FREETEXT, NULL, NULL, NULL);

    QueryNode *last = root;
    while(!trd.eof())
    {
        char *tstr = trd.readTerm();
        if(!tstr) break;
        talloc_steal(tstr, memctx);

        QueryNode *term = makeNode(QueryNode::TERM, NULL, NULL, tstr);

        last->rhs = term;
        last = term;
    }

    if(!trd.eof()) {
        trd.seek(where);
        return NULL;
    }

    return root;
}

QueryNode *QueryParser::parseQuery()
{
    QueryNode *root = NULL;
    if(!root) root = parsePhrase();
    if(!root) root = parseFreeText();
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
    int termCount, rqCount;

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
                node->termText, node->termId, node->postingCount);
            return;
        case QueryNode::OR:
            printf("OR, expected: %d\n", node->estimPostings);
            dumpQueryTree(node->lhs, 3+indent);
            dumpQueryTree(node->rhs, 3+indent);
            return;
        case QueryNode::AND:
            printf("AND, expected: %d\n", node->estimPostings);
            dumpQueryTree(node->lhs, 3+indent);
            dumpQueryTree(node->rhs, 3+indent);
            return;
        case QueryNode::FREETEXT:
            printf("FREETEXT\n");
            for(const QueryNode *p = node->rhs; p; p = p->rhs)
                dumpQueryTree(p, 3+indent);
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
        node->termId =
            dictionary.lookup(node->termText, strlen(node->termText));

        if(node->termId == -1) 
            info("term »%s« does not appear in dictionary\n",
                 node->termText);
        else {
            node->postingsKey =
                dictionary.getPostingsKey(node->termId, idxtype);
            Dictionary::PostingsInfo info =
                node->postingsKey.getPostingsInfo();
            node->postingCount = info.postingCount;

            info("resolved term »%s« (%d): postings id 0x%08x, has %d postings at 0x%08llx, size %zu\n",
                  node->termText, node->termId, (int)node->postingsKey, node->postingCount, (unsigned long long)info.offset, info.size);
        }

        if(node->postingCount == 0)
            node->postings = &emptyPostingList;
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
    rqCount = 0;
    for(int i=0; i<termCount; i++)
    {
        if(terms[i]->postings)
            continue;

        bool dupli = false;
        for(int j=0; j<i; j++)
            if(terms[j]->postingsKey == terms[i]->postingsKey) {
                info("term %d is duplicate of term %d\n", i,j);
                dupli = true;
                break;
            }
        if(dupli) continue;

        PostingsSource::ReadRq *rq = rqs + (rqCount++);
        rq->postingsKey = terms[i]->postingsKey;
        rq->data = talloc_size(memctx, rq->postingsKey.getSize());
        assert(rq->data);
    }
}

/* -------------------------------------------------------------------------- */

class BooleanQueryEngine : public QueryEngineBase
{
    QueryNode **scratchpad;
    int nodeCount, stopwordCount;

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
        return dictionary.isStopWord(node->termId);
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
        node->estimPostings = node->postingCount;
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
            if((*p)->estimPostings == 0) {
                info("found empty node %p in AND group, emptying whole group\n", (*p));
                *node = emptyQueryNode;
                scratchpad = base;
                return node;
            }
    } else {
        for(QueryNode **p = children; p<end; ) 
            if((*p)->estimPostings == 0) {
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
            if((*q)->type == (*p)->type && (*q)->postingsKey == (*p)->postingsKey) {
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
    if(type == QueryNode::AND && stopwordCount*2 < termCount) 
        for(QueryNode **p = children; p<end; )
            if((*p)->type != QueryNode::TERM ||
               !dictionary.isStopWord((*p)->termId))
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
        /* end[-1] is to be the child with smallest estimPostings and
           end[-2] is to be the child with second-smalest estimPostings */
        if(end[-1]->estimPostings > end[-2]->estimPostings)
            std::swap(end[-1], end[-2]);

        for(QueryNode **p = children; p<end-2; p++)
            if((*p)->estimPostings < end[-1]->estimPostings) {
                std::swap(end[-1], end[-2]);
                std::swap(*p, end[-1]);
            }
            else if((*p)->estimPostings < end[-2]->estimPostings)
                std::swap(*p, end[-2]);

        /* set up the new node (reusing one of the old ones)
           the deeper sub-tree always goes to the lhs. */
        QueryNode *v = *pool++;
        v->lhs = end[-1];
        v->rhs = end[-2];
        
        if(v->lhs->depth < v->rhs->depth)
            std::swap(v->lhs, v->rhs);
        v->depth = v->lhs->depth + 1;

        v->estimPostings =
            type == QueryNode::OR
                ? v->lhs->estimPostings + v->rhs->estimPostings
                : std::min(v->lhs->estimPostings, v->rhs->estimPostings);

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
    int rqDone = 0;

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + (rqDone++);

            Dictionary::PostingsInfo info =
                rq->postingsKey.getPostingsInfo();
            int *decoded =
                decodePostings(Reader(rq->data, info.size), info.postingCount);

            info("processing posting list 0x%08x\n", (int)rq->postingsKey);

            for(int i=0; i<termCount; i++) 
                if(terms[i]->postingsKey == rq->postingsKey) {
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
    rd.read_uv(); /* n_pos */
    for(int i=1; i<count; i++) {
        ret[i] = ret[i-1] + rd.read_uv(); /* document id */
        rd.read_uv(); /* n_pos */
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

    info("got %d postings\n", node->postingCount);

    evaluateNode(node->parent);
}

void BooleanQueryEngine::evaluateAndNode(QueryNode *node)
{
    if(node->lhs->postingCount == 0 ||
       node->rhs->postingCount == 0) {
        node->postings = &emptyPostingList;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->postingCount, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->postingCount, *Bend = B + m;

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

    node->postingCount = (C - (int *)node->postings);
}

void BooleanQueryEngine::evaluateOrNode(QueryNode *node)
{
    if(node->lhs->postingCount == 0) {
        node->postings = node->rhs->postings;
        node->postingCount = node->rhs->postingCount;
        return;
    }
    if(node->rhs->postingCount == 0) {
        node->postings = node->lhs->postings;
        node->postingCount = node->lhs->postingCount;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->postingCount, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->postingCount, *Bend = B + m;

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

    node->postingCount = (C - (int *)node->postings) + (Aend - A) + (Bend - B);
}

void BooleanQueryEngine::printResult(const QueryNode *root)
{
    const int *postings = (const int *)root->postings;
    assert(postings);

    printf("QUERY: %s TOTAL: %d\n", queryText, root->postingCount);
    if(!noResults)
        for(int i=0; i<root->postingCount; i++) 
            puts(artitles.getTitle(postings[i]));
}

void BooleanQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);

    resolveTerms(root, LEMMATIZED);
    
    dumpQueryTree(root, "raw query:");

    nodeCount = countNodes(root);
    stopwordCount = countStopwords(root);

    QueryNode *_scratchpad[nodeCount]; scratchpad = _scratchpad;

    termCount = countTerms(root);
    linearize(root);
    root = optimize(root);
    fixParents(root);

    dumpQueryTree(root, "optimized query:");
    
    termCount = countTerms(root);
    QueryNode *_terms[termCount]; terms = _terms;
    PostingsSource::ReadRq _rqs[termCount]; rqs = _rqs;

    extractTerms(root, terms);
    createRqs();
    posrc.request(rqs, rqCount);
    processPostings();
    printResult(root);
}


/* -------------------------------------------------------------------------- */

class PhraseQueryEngine : public QueryEngineBase
{
    struct Doc {
        int docId, positionsOffs;
    };

    typedef unsigned short pos_t;
    
    int *offsets;
    struct Doc *docs, *docsRdptr, *docsWrptr, *docsEnd;
    pos_t *positions, *positionsWrptr;

    inline void sortTerms();
    inline void processPostings();
    void processTerm(Reader rd, int postingCount, int offset) __attribute__((hot));
    inline void processDocument(int docId, Reader prd, int offset) __attribute__((hot));
    inline void makeWorkingSet(Reader rd, int postingCount, int offset) __attribute__((hot));
    inline void printResult();
    
    void do_run(QueryNode *root);

public:
    static inline void run(QueryNode *root) {
        PhraseQueryEngine().do_run(root);
    }
};

void PhraseQueryEngine::sortTerms()
{
    for(int i=0; i<termCount; i++)
        offsets[i] = termCount-1-i;

    for(int i=0; i<termCount; i++)
        for(int j=i; j > 0 && terms[j]->postingCount < terms[j-1]->postingCount; j--) {
            std::swap(terms[j], terms[j-1]);
            std::swap(offsets[j], offsets[j-1]);
        }
}

void PhraseQueryEngine::processPostings()
{
    int rqDone = 0;

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + (rqDone++);

            Dictionary::PostingsInfo info =
                rq->postingsKey.getPostingsInfo();
            Reader rd(rq->data, info.size);

            info("processing posting list 0x%08x\n", (int)rq->postingsKey);

            for(int i=0; i<termCount; i++) 
                if(terms[i]->postingsKey == rq->postingsKey)
                    processTerm(rd, info.postingCount, offsets[i]);
        }
}

void PhraseQueryEngine::processTerm(Reader rd, int postingCount, int offset)
{
    /* check if we have something to merge with */
    if(unlikely(!docs)) {
        makeWorkingSet(rd, postingCount, offset);
        return;
    }

    /* but the figers at start */
    docsRdptr = docsWrptr = docs;
    positionsWrptr = positions;

    /* where the positions data in bytestream start */
    int positionsOffset = 4 + rd.read_u32();

    /* read the first document info */
    int docId = rd.read_u24(),
        positionsSize = rd.read_uv();

    /* while there are documents left both in bytestream and in our working set */
    while(postingCount-- && docsRdptr < docsEnd)
    {
        /* create the reader for positions and process the document */
        Reader prd((const char *)rd.buffer() + positionsOffset, positionsSize);
        processDocument(docId, prd, offset);

        /* move on to the next document */
        positionsOffset += positionsSize;
        if(postingCount) {
            docId += rd.read_uv();
            positionsSize = rd.read_uv();
        }
    }

    /* remember how many documents we have */
    docsEnd = docsWrptr;
}

void PhraseQueryEngine::processDocument(int docId, Reader prd, int offset)
{
    /* skip past any documents earlier than this */
    while(docsRdptr < docsEnd && docsRdptr->docId < docId)
        docsRdptr++;

    /* check if our document has been found */
    if(docsRdptr >= docsEnd || docsRdptr->docId > docId)
        return;

    /* fetch the document, remember where its positions start */
    struct Doc doc = *docsRdptr;
    const pos_t *positionsRdptr = positions + doc.positionsOffs;

    /* move the positions to the current writing finger */
    doc.positionsOffs = positionsWrptr - positions;

    /* merge positions. all of them are incremented by 'offset' */
    pos_t p = offset;
    while(!prd.eof() && *positionsRdptr != (pos_t)-1)
    {
        /* get the next position */
        p += prd.read_uv();
        /* skip past position earlier than this */
        while(*positionsRdptr < p) positionsRdptr++;
        /* write the position back, if found */
        if(*positionsRdptr == p)
            *positionsWrptr++ = p;
    }

    /* if any position was written, terminate the positions
     * list with maxval and write the document back */
    if(positionsWrptr - positions > doc.positionsOffs) {
        *positionsWrptr++ = (pos_t)-1;
        *docsWrptr++ = doc;
    }
}


void PhraseQueryEngine::makeWorkingSet(Reader rd, int postingCount, int offset)
{
    /* find out where positions start */
    int positionsOffset = 4 + rd.read_u32();

    /* allocate memory (positions may be bigger than necessary) */
    docs = talloc_array(memctx, struct Doc, postingCount);
    positions = talloc_array(memctx, pos_t, rd.size() - positionsOffset + postingCount);
    assert(docs && positions);

    /* set up writing fingers */
    docsWrptr = docs;
    positionsWrptr = positions;

    /* read the first document info */
    int docId = rd.read_u24(),
        positionsSize = rd.read_uv();

    /* while there are documents left both in bytestream and in our buffer */
    while(postingCount--)
    {
        /* create the reader for positions and process the document */
        Reader prd((const char *)rd.buffer() + positionsOffset, positionsSize);

        /* write the doc */
        docsWrptr->docId = docId;
        docsWrptr->positionsOffs = positionsWrptr - positions;
        docsWrptr++;

        /* write the positions */
        pos_t p = offset;
        while(!prd.eof()) {
            p += prd.read_uv();
            *positionsWrptr++ = p;
        }
        /* terminate the positions list with maxval */
        *positionsWrptr++ = (pos_t)-1;

        /* move on to the next document */
        positionsOffset += positionsSize;
        if(postingCount) {
            docId += rd.read_uv();
            positionsSize = rd.read_uv();
        }
    }

    /* remember how many documents we have */
    docsEnd = docsWrptr;
}

void PhraseQueryEngine::printResult()
{
    int documentCount = docsEnd - docs;
    printf("QUERY: %s TOTAL: %d\n", queryText, documentCount);
    if(!noResults)
        for(int i=0; i<documentCount; i++) 
            puts(artitles.getTitle(docs[i].docId));
}

void PhraseQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);
    docs = docsRdptr = docsWrptr = docsEnd = NULL;
    positions = positionsWrptr = NULL;

    resolveTerms(root, POSITIONAL);
    
    dumpQueryTree(root, "raw query:");

    termCount = countTerms(root);

    QueryNode *_terms[termCount]; terms = _terms;
    PostingsSource::ReadRq _rqs[termCount]; rqs = _rqs;
    int _offsets[termCount]; offsets = _offsets;

    extractTerms(root, terms);
    sortTerms();

    if(termCount && terms[0]->postingsKey.getPostingCount()) {
        createRqs();
        posrc.request(rqs, rqCount);
        processPostings();
    }

    printResult();
}


/* -------------------------------------------------------------------------- */

class FreeTextQueryEngine : public QueryEngineBase
{
    struct Doc {
        int docId;
        float weight;
        inline float score() const {
            float pagerank = artitles.getPageRank(docId);
            return freetextWeights.alpha * weight +
                   freetextWeights.beta * pagerank +
                   freetextWeights.gamma * weight * pagerank;
        }
        inline bool operator<(const struct Doc &d) const {
            float a = score(), b = d.score();
            return likely(a != b)
                    ? a > b
                    : docId < docId;
        }
    };

    struct TermWeights {
        float queryTf, idf;
    };

    float queryWeight;
    struct TermWeights *weights;
    struct Doc *docs;
    int docCount;

    static int markedDocCount, *markedDocs;

    static inline void fetchMarkedDocs();
    inline void prepareTerms();
    inline void processPostings();
    inline struct Doc *decodePostings(Reader rd, int count, struct TermWeights w);
    void trimPostings(struct Doc *postings, int *ppostingCount);
    void processTerm(struct Doc *postings, int postingCount);
    inline void sortResult();
    inline void printResult();
    
    void do_run(QueryNode *root);

public:
    static inline void run(QueryNode *root) {
        FreeTextQueryEngine().do_run(root);
    }
};

int FreeTextQueryEngine::markedDocCount = -1,
    *FreeTextQueryEngine::markedDocs = NULL; /* GLOBAL (ehh, static actually) */

void FreeTextQueryEngine::fetchMarkedDocs()
{
    if(!onlyMarkedDocs || markedDocCount >= 0)
        return;

    info("fetching marked documents\n");

    int markerTermId = dictionary.lookup("\"m", 2);
    if(markerTermId == -1) {
        info("marker term does not appear in dictionary\n");
        markedDocCount = 0;
        return;
    }

    Dictionary::Key key = dictionary.getPostingsKey(markerTermId, LEMMATIZED);
    Dictionary::PostingsInfo info = key.getPostingsInfo();

    info("resolved marker term (%d): postings id 0x%08x, has %d postings at 0x%08llx, size %zu\n",
          markerTermId, (int)key, info.postingCount, (unsigned long long)info.offset, info.size);

    PostingsSource::ReadRq rq;
    rq.postingsKey = key;
    rq.data = talloc_size(NULL, info.size);
    assert(rq.data);

    posrc.request(&rq, 1);
    int done = posrc.wait();
    assert(done == 1);

    markedDocCount = info.postingCount;
    markedDocs = talloc_array(NULL, int, markedDocCount);
    assert(markedDocs);

    Reader rd(rq.data, info.size);
    for(int i=0; i<markedDocCount; i++) {
        markedDocs[i] =
            (i == 0) ? rd.read_u24() : markedDocs[i-1] + rd.read_uv();
        rd.read_uv();
    }

    talloc_free(rq.data);
}

void FreeTextQueryEngine::prepareTerms()
{
    for(int i=0; i<termCount; i++)
        for(int j=i; j > 0 && terms[j]->postingCount < terms[j-1]->postingCount; j--) 
            std::swap(terms[j], terms[j-1]);

    int tc = termCount;
    termCount = 0;

    for(int i=0; i<tc; i++)
    {
        int repcnt = 1;
        while(i+1 < tc && terms[i+1]->postingsKey == terms[i]->postingsKey)
            i++, repcnt++;

        terms[termCount] = terms[i];

        weights[termCount].idf =
            logf((float)artitles.size() /
                 (float)terms[i]->postingsKey.getPostingCount());
        weights[termCount].queryTf =
            (float)repcnt / (float)tc;
        
        termCount++;
    }

    queryWeight = 0;
    for(int i=0; i<termCount; i++)
        queryWeight += square(weights[i].queryTf * weights[i].idf);
    queryWeight = 1.f/sqrtf(queryWeight);
}

void FreeTextQueryEngine::processPostings()
{
    int rqDone = 0;

    while(int count = posrc.wait())
        while(count--)
        {
            const PostingsSource::ReadRq *rq = rqs + rqDone;
            assert(terms[rqDone]->postingsKey == rq->postingsKey);

            Dictionary::PostingsInfo info =
                rq->postingsKey.getPostingsInfo();
            struct Doc *decoded =
                decodePostings(Reader(rq->data, info.size), info.postingCount, weights[rqDone]);

            info("processing posting list 0x%08x\n", (int)rq->postingsKey);

            int postingCount = info.postingCount;
            trimPostings(decoded, &postingCount);
            
            processTerm(decoded, postingCount);

            rqDone++;
        }
}

FreeTextQueryEngine::Doc *FreeTextQueryEngine::decodePostings(Reader rd, int count, struct TermWeights w)
{
    if(count == 0)
        return NULL;

    struct Doc *ret = talloc_array(memctx, struct Doc, count);
    assert(ret);

    for(int i=0; i<count; i++)
    {
        ret[i].docId =
            (i == 0) ? rd.read_u24() : ret[i-1].docId + rd.read_uv();
        float docTf =
            (float)rd.read_uv() / (float)artitles.getTermCount(ret[i].docId);
        float docWeight = artitles.getTfIdfWeight(ret[i].docId);
        ret[i].weight = w.queryTf*w.idf * docTf*w.idf * queryWeight * docWeight;
    }

    return ret;
}

void FreeTextQueryEngine::trimPostings(struct Doc *postings, int *ppostingCount)
{
    if(!onlyMarkedDocs)
        return;

    int n = *ppostingCount, m = markedDocCount;
    struct Doc *A = postings, *Aend = postings+n, *C = postings;
    const int *B = markedDocs, *Bend = B+m;
    
    while(A < Aend && B < Bend)
        if(A->docId < *B)
            A++;
        else if(A->docId > *B)
            B++;
        else
            *C++ = *A, A++, B++;

    *ppostingCount = C - postings;
}

void FreeTextQueryEngine::processTerm(struct Doc *postings, int postingCount)
{
    if(!docs) {
        docs = postings;
        docCount = postingCount;
        return;
    }
    if(!postings)
        return;

    int n = postingCount, m = docCount;
    const struct Doc *A = postings, *Aend = A+n,
                     *B = docs, *Bend = B+m;

    struct Doc *C = talloc_array(memctx, struct Doc, n+m);
    docs = C;
    assert(C);
    
    while(A < Aend && B < Bend)
        if(A->docId < B->docId)
            *C++ = *A++;
        else if(A->docId > B->docId)
            *C++ = *B++;
        else {
            C->docId = A->docId;
            C->weight = A->weight + B->weight;
            C++, A++, B++;
        }
    if(A != Aend)
        memcpy(C, A, (Aend - A) * sizeof(struct Doc));
    if(B != Bend)
        memcpy(C, B, (Bend - B) * sizeof(struct Doc));

    docCount = (C - docs) + (Aend - A) + (Bend - B);
}

void FreeTextQueryEngine::sortResult()
{
    std::sort(docs, docs+docCount);
}

void FreeTextQueryEngine::printResult()
{
    int n = onlyBestDocs && onlyBestDocs < docCount ? onlyBestDocs : docCount;
    printf("QUERY: %s TOTAL: %d\n", queryText, docCount);
    if(noResults)
        return;
    if(verbose)
        for(int i=0; i<n; i++) 
            printf("%d: %s (%.2f%%, pagerank: %.3f, score: %e)\n", 
                    i+1, artitles.getTitle(docs[i].docId),
                    100.f*docs[i].weight, artitles.getPageRank(docs[i].docId), docs[i].score());
    else
        for(int i=0; i<n; i++) 
            puts(artitles.getTitle(docs[i].docId));
}

void FreeTextQueryEngine::do_run(QueryNode *root)
{
    memctx = talloc_parent(root);
    weights = NULL;
    docs = NULL;
    docCount = 0;

    fetchMarkedDocs();

    resolveTerms(root, LEMMATIZED);
    
    dumpQueryTree(root, "raw query:");

    termCount = countTerms(root);

    QueryNode *_terms[termCount]; terms = _terms;
    PostingsSource::ReadRq _rqs[termCount]; rqs = _rqs;
    struct TermWeights _weights[termCount]; weights = _weights;

    extractTerms(root, terms);
    prepareTerms();
    createRqs();
    posrc.request(rqs, rqCount);
    processPostings();
    sortResult();
    printResult();
}


/* -------------------------------------------------------------------------- */

static void print_usage(const char *progname, const char *reason) __attribute__((noreturn));
static void print_usage(const char *progname, const char *reason) {
    if(reason)
        fprintf(stderr, "%s: %s\n", progname, reason);
    fprintf(stderr, "usage: search [-v] [-r] [-m] [-b x] [-h] [-f x:y:z]\n"
                    "  -v: print verbose progress information\n"
                    "  -r: do not print title results\n"
                    "  -m: consider only marked documents\n"
                    "  -b: show only x best matches\n"
                    "  -f: specify free text weighting factors\n"
                    "  -h: print this help message\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    while(int opt = getopt(argc, argv, "vrmb:f:h"))
        if(opt == -1) break;
        else switch(opt) {
            case 'v': verbose = true; break;
            case 'r': noResults = true; break;
            case 'm': onlyMarkedDocs = true; break;
            case 'b': {
                char *end;
                onlyBestDocs = strtol(optarg, &end, 0);
                if(end == optarg || *end != '\0') print_usage(argv[0], "unable to parse switch argument -- 'b'");
                break;
            }
            case 'f': {
                char *end1, *end2, *end3;
                freetextWeights.alpha = strtof(optarg, &end1);
                if(end1 == optarg || *end1 != ':') print_usage(argv[0], "unable to parse switch argument -- 'f'");
                freetextWeights.beta = strtof(end1+1, &end2);
                if(end2 == end1+1 || *end2 != ':') print_usage(argv[0], "unable to parse switch argument -- 'f'");
                freetextWeights.gamma = strtof(end2+1, &end3);
                if(end3 == end2+1 || *end3 != '\0') print_usage(argv[0], "unable to parse switch argument -- 'f'");
                break;
            }
            case 'h':
            default: print_usage(NULL, NULL);
        }
    if(optind < argc) print_usage(argv[0], "unrecognized switches");

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
        else if(root->type == QueryNode::FREETEXT)
            FreeTextQueryEngine::run(root);
        else
            BooleanQueryEngine::run(root);
        talloc_free(talloc_parent(root));

        info("--- total time: %.3lf ms wall / %.3lf ms cpu\n",
             wallTime.end()*1000.f, cpuTime.end()*1000.f);
    }

    return 0;
}
