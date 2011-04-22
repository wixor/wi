#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <talloc.h>
#include "bufrw.h"
#include "term.h"

/* -------------------------------------------------------------------------- */

#define info(fmt, ...) printf(fmt, ## __VA_ARGS__)

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

    int fd = open(filename, O_RDONLY);
    if(fd == -1) {
        perror("opening article titles file failed: open(2)");
        abort();
    }

    FileIO fio(fd);
    off_t size = fio.seek(0, SEEK_END);

    char *data = (char *)fio.read_raw_alloc(size, 0);
    talloc_steal(memctx, data);
    Reader rd(data, size);

    assert(rd.read_u32() == 0x4c544954);
    int n_articles = rd.read_u32();

    titles = talloc_array(memctx, char *, n_articles);
    assert(titles);
    for(int i=0; i<n_articles; i++) {
        titles[i] = data + rd.tell();
        rd.seek_past('\0');
    }

    close(fd);
    info("article titles read in %.03lf seconds.\n", timer.end());
}

/* -------------------------------------------------------------------------- */

enum IndexType {
    POSITIONAL, LEMMATIZED
};

/* -------------------------------------------------------------------------- */

class Dictionary
{
    struct PostingList {
        off_t offset;
        int n_entries;
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
        int n_entries;
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

    int fd = open(filename, O_RDONLY);
    if(fd == -1) {
        perror("opening dictionary file failed: open(2)");
        abort();
    }

    off_t size = lseek(fd, 0, SEEK_END);
    assert(size != (off_t)-1);

    void *data =
        mmap(NULL, size, PROT_READ, MAP_PRIVATE|MAP_POPULATE, fd, 0);
    if(data== NULL) {
        perror("mmmaping dictionary file failed: mmap(2)");
        abort();
    }

    do_read(Reader(data, size));

    munmap(data, size);
    close(fd);
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
    positional[0].offset = 0;
    for(int i=1; i<=term_count; i++) {
        positional[i].offset = positional[i-1].offset + rd.read_uv();;
        positional[i-1].n_entries = rd.read_uv();
    }

    /* read lemmatized list info */
    lemmatized[0].offset = 0;
    for(int i=1; i<=lemmatized_list_count; i++) {
        lemmatized[i].offset = lemmatized[i-1].offset + rd.read_uv();;
        lemmatized[i-1].n_entries = rd.read_uv();
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
    ret.n_entries = lists[k].n_entries;

    return ret;
}

/* -------------------------------------------------------------------------- */

class PostingsSource
{
public:
    struct ReadRq {
        Dictionary::PostingListInfo info;
        void *data;
    };

private:
    void *memctx;
    int evfd;
    int lemmatized_fd, positional_fd;

    pthread_t thread;
    pthread_mutex_t lock;
    ReadRq *rqs;
    int rqs_count, fd;

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
        eventfd_read(evfd, &foo);

        pthread_mutex_lock(&lock);

        FileIO fio(fd);
        for(int i=0; i<rqs_count; i++)
        {
            rqs[i].data = fio.read_raw_alloc(rqs[i].info.size, rqs[i].info.offset);
            talloc_steal(memctx, rqs[i].data);

            eventfd_write(evfd, 1);
            info("finished postings request %d\n", i);
        }
        fd = -1;

        pthread_mutex_unlock(&lock);
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
    int fd = open(filename, O_RDONLY);
    if(fd == -1) {
        perror("failed to open index file: open(2)");
        abort();
    }

    uint32_t mg;
    FileIO fio(fd);
    fio.read_raw(&mg, sizeof(mg));
    assert(mg == magic);

    return fd;
}

void PostingsSource::start(const char *positional_filename, const char *lemmatized_filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "postings reader");
    assert(memctx);
    
    info("starting postings source...\n");

    evfd = eventfd(0, 0);
    assert(evfd != -1);

    positional_fd = openIndex(positional_filename, 0x50584449);
    lemmatized_fd = openIndex(lemmatized_filename, 0x4c584449);
    fd = -1;

    pthread_mutex_init(&lock, NULL);
    assert(pthread_create(&thread, NULL, &runThreadFunc, this) == 0);

    info("postings source running\n");
}

void PostingsSource::stop()
{
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    pthread_mutex_destroy(&lock);
    talloc_free(memctx);
}

void PostingsSource::request(ReadRq *rqs, int count, IndexType idxtype)
{
    info("requesting %d postings read from %s index:\n",
            count, idxtype == LEMMATIZED ? "lemmatized" : "positional");
    for(int i=0; i<count; i++)
        info("  %zu bytes from 0x%llx\n", rqs[i].info.size, rqs[i].info.offset);

    pthread_mutex_lock(&lock);
    fd = idxtype == POSITIONAL ? positional_fd : lemmatized_fd;
    this->rqs = rqs;
    rqs_count = count;
    eventfd_write(evfd, 1);
    pthread_mutex_unlock(&lock);
}

int PostingsSource::wait(void)
{
    if(fd == -1)
        return 0;

    uint64_t val;
    eventfd_read(evfd, &val);
    info("%d postings read\n", (int)val);
    return val;
}

/* -------------------------------------------------------------------------- */

struct QueryNode
{
    enum Type { AND, OR, PHRASE, TERM } type;

    QueryNode *lhs, *rhs, *parent;

    const char *term_text; /* actual term text */
    Dictionary::Key term_id; /* index of term in dictionary */

    Dictionary::PostingListInfo info;
    void *postings; /* actual posting list */
};

/* -------------------------------------------------------------------------- */

/* GLOBALS */
static Artitles artitles;
static Dictionary dictionary;
static PostingsSource posrdr;
static long empty_posting_list = -42L;

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

public:
    QueryNode *run(const char *query);
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

QueryNode *QueryParser::run(const char *query)
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

static void resolve_terms(QueryNode *node, IndexType idxtype)
{
    if(!node) return;
    if(node->type == QueryNode::TERM)
    {
        node->term_id =
            dictionary.lookup(node->term_text, strlen(node->term_text), idxtype);

        if(node->term_id != -1)
            node->info = dictionary.getPostingListInfo(node->term_id);

        if(node->info.n_entries == 0)
            node->postings = &empty_posting_list;

        info("term '%s' resolved to id 0x%08x, has %d entries at 0x%llx, size %zu\n",
             node->term_text, node->term_id,
             node->info.n_entries, node->info.offset, node->info.size);

    } else {
        resolve_terms(node->lhs, idxtype);
        resolve_terms(node->rhs, idxtype);
    }
}

/* -------------------------------------------------------------------------- */

class PostingsDecoder
{
    static inline void *decode_lemmatized(Reader rd, int count);
    static inline void *decode_positional(Reader rd, int count);
public:
    static inline void *decode(Reader rd, int count, IndexType idxtype);
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

void *PostingsDecoder::decode(Reader rd, int count, IndexType idxtype)
{
    switch(idxtype) {
        case LEMMATIZED: return decode_lemmatized(rd, count);
        case POSITIONAL: return decode_positional(rd, count);
    }
    abort();
}

/* -------------------------------------------------------------------------- */

class TreePostingsSource
{
    void *memctx;

    IndexType idxtype;

    int term_count, done_wridx, done_rdidx;
    QueryNode **terms, **done;

    int rqs_count, rqs_rdidx;
    PostingsSource::ReadRq *rqs;
    int *term_rq_map;

    static int countLeaves(QueryNode *node);
    static int extractLeaves(QueryNode *node, QueryNode **ptr);
    void markFilledTermsAsDone();
    void createRqs();

public:
    void attach(QueryNode *root, IndexType idxtype, void *memctx);
    QueryNode *wait();
};

void TreePostingsSource::attach(QueryNode *root, IndexType idxtype, void *memctx)
{
    this->idxtype = idxtype;
    this->memctx = memctx;

    done_rdidx = done_wridx = rqs_count = rqs_rdidx = 0;
    term_count = countLeaves(root);
    terms = talloc_array(memctx, QueryNode*, term_count);
    done = talloc_array(memctx, QueryNode*, term_count);
    assert(terms && done);

    extractLeaves(root, terms);
    markFilledTermsAsDone();
    createRqs();
    posrdr.request(rqs, rqs_count, idxtype);
}

QueryNode *TreePostingsSource::wait()
{
    if(done_rdidx < done_wridx)
        return done[done_rdidx++];

    int n = posrdr.wait();
    if(n == 0)
        return NULL;

    while(n--)
    {
        PostingsSource::ReadRq *rq = &rqs[rqs_rdidx];
        talloc_steal(memctx, rq->data);

        void *decoded = PostingsDecoder::decode(
            Reader(rq->data, rq->info.size), rq->info.n_entries, idxtype);
        talloc_steal(memctx, decoded);

        for(int i=0; i<term_count; i++)
            if(term_rq_map[i] == rqs_rdidx) {
                info("term %d ('%s', id 0x%08x) got data\n",
                        i, terms[i]->term_text, terms[i]->term_id);
                terms[i]->postings = decoded;
                done[done_wridx++] = terms[i];
            }

        rqs_rdidx++;
    }

    assert(done_rdidx < done_wridx);
    return done[done_rdidx++];
}

int TreePostingsSource::countLeaves(QueryNode *node)
{
    if(!node) return 0;
    if(node->type == QueryNode::TERM)
        return 1;
    return countLeaves(node->lhs) + countLeaves(node->rhs);
}
int TreePostingsSource::extractLeaves(QueryNode *node, QueryNode **ptr)
{
    if(!node) return 0;
    if(node->type == QueryNode::TERM) {
        *ptr = node;
        return 1;
    }
    int l = extractLeaves(node->lhs, ptr);
    int r = extractLeaves(node->rhs, ptr+l);
    return l+r;
}
void TreePostingsSource::markFilledTermsAsDone()
{
    int filled = 0;
    for(int i=0,j=0; i<term_count; i++)
    {
        if(terms[i]->postings) {
            done[done_wridx++] = terms[i];
            filled++;
            info("removing term '%s' (0x%08x), no postings\n",
                  terms[i]->term_text, terms[i]->term_id);
        } else
            terms[j++] = terms[i];
    }
    term_count -= filled;
}
void TreePostingsSource::createRqs()
{
    rqs = talloc_array(memctx, PostingsSource::ReadRq, term_count);
    assert(rqs);

    for(int i=0; i<term_count; i++)
    {
        bool dupli = false;
        for(int j=0; j<i; j++)
            if(terms[j]->term_id == terms[i]->term_id) {
                term_rq_map[i] = term_rq_map[j];
                info("term %d: duplicate of term %d\n", i, j);
                dupli = true;
                break;
            }
        if(dupli) continue;

        info("term %d: '%s', id 0x%08x, postings at 0x%llx, size %zu\n",
             i, terms[i]->term_text,
             terms[i]->term_id, terms[i]->info.offset, terms[i]->info.size);

        term_rq_map[i] = rqs_count++;
        rqs[term_rq_map[i]].info = terms[i]->info;
    }
}

/* -------------------------------------------------------------------------- */

class BooleanQueryEngine
{
    void *memctx;

    void evaluateAndNode(QueryNode *node) __attribute__((hot));
    void evaluateOrNode(QueryNode *node) __attribute__((hot));
    inline void evaluateNode(QueryNode *node);

public:
    void run(QueryNode *root);
};

void BooleanQueryEngine::evaluateAndNode(QueryNode *node)
{
    if(node->lhs->info.n_entries == 0 ||
       node->rhs->info.n_entries == 0) {
        node->postings = &empty_posting_list;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->info.n_entries, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->info.n_entries, *Bend = B + m;
    int *C = (int *)node->postings;

    node->postings = talloc_array(memctx, int, n < m ? n : m);
    assert(node->postings);

    while(A < Aend && B < Bend)
        if(*A < *B)
            A++;
        else if(*A > *B)
            B++;
        else 
            *C++ = *A, A++, B++;
    if(A != Aend)
        memcpy(C, A, (Aend - A) * sizeof(int));
    if(B != Bend)
        memcpy(C, B, (Bend - B) * sizeof(int));

    node->info.n_entries = (C - (int *)node->postings) + (Aend - A) + (Bend - B);
}

void BooleanQueryEngine::evaluateOrNode(QueryNode *node)
{
    if(node->lhs->info.n_entries == 0) {
        node->postings = node->rhs->postings;
        node->info.n_entries = node->rhs->info.n_entries;
        return;
    }
    if(node->rhs->info.n_entries == 0) {
        node->postings = node->lhs->postings;
        node->info.n_entries = node->lhs->info.n_entries;
        return;
    }

    const int *A = (int *)node->lhs->postings, n = node->lhs->info.n_entries, *Aend = A + n,
              *B = (int *)node->rhs->postings, m = node->rhs->info.n_entries, *Bend = B + m;
    int *C = (int *)node->postings;

    node->postings = talloc_array(memctx, int, n + m);
    assert(node->postings);

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

    node->info.n_entries = (C - (int *)node->postings) + (Aend - A) + (Bend - B);
}

void BooleanQueryEngine::evaluateNode(QueryNode *node)
{
    info("evaluating node %p\n", node);

    if(node->type == QueryNode::AND)
        evaluateAndNode(node);
    else if(node->type == QueryNode::OR)
        evaluateOrNode(node);
    else
        abort();

    info("result is %d postings\n", node->info.n_entries);
}

void BooleanQueryEngine::run(QueryNode *root)
{
    memctx = talloc_parent(root);

    resolve_terms(root, LEMMATIZED);

    TreePostingsSource tpr;
    tpr.attach(root, LEMMATIZED, memctx);

    while(QueryNode *node = tpr.wait())
    {
        info("node %p (term '%s', id 0x%08x) ready to go\n",
              node, node->term_text, node->term_id);

        node = node->parent;
        while(node && node->lhs->postings && node->rhs->postings) {
            evaluateNode(node);
            node = node->parent;
        }
    }

    assert(root->postings);
    printf("--- RESULTS: %d pages\n", root->info.n_entries);
    for(int i=0; i<root->info.n_entries; i++)
        printf("  %d: %s\n", i+1, artitles.lookup(((int *)root->postings)[i]));
}

/* -------------------------------------------------------------------------- */

class PhraseQueryEngine
{
    void *memctx;

public:
    void run(QueryNode *root);
};

void PhraseQueryEngine::run(QueryNode *root)
{
    memctx = talloc_parent(root);
}

/* -------------------------------------------------------------------------- */

static void dump_query_tree(const QueryNode *node, int indent = 0)
{
    static const char spaces[] = "                                                                                ";
    info("%.*s %p: ", indent, spaces, node);
    if(!node) {
        info("NULL\n");
        return;
    }
    switch(node->type) {
        case QueryNode::TERM:
            info("TERM: »%s«\n", node->term_text);
            return;
        case QueryNode::OR:
            info("OR\n");
            dump_query_tree(node->lhs, 3+indent);
            dump_query_tree(node->rhs, 3+indent);
            return;
        case QueryNode::AND:
            info("AND\n");
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
    QueryParser qp;
    QueryNode *root = qp.run(query);
    if(!root)
        return;

    dump_query_tree(root);

    if(root->type == QueryNode::PHRASE) {
        PhraseQueryEngine e;
        e.run(root);
    } else {
        BooleanQueryEngine e;
        e.run(root);
    }

    talloc_free(talloc_parent(root));
}

int main(void)
{
    artitles.read("db/artitles");
    dictionary.read("db/dictionary");
    posrdr.start("db/positional", "db/lemmatized");

    for(;;) {
        static char buffer[1024];
        printf("Enter query: "); fflush(stdout);
        if(fgets(buffer, 1023, stdin) == NULL)
            break;
        run_query(buffer);
    }

    return 0;
}
