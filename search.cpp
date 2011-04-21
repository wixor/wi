#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <talloc.h>
#include "bufrw.h"
#include "term.h"

/* -------------------------------------------------------------------------- */

#define info(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)

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

struct QueryNode
{
    enum Type { AND, OR, PHRASE, TERM } type;

    QueryNode *lhs, *rhs, *parent;

    const char *term_text; /* actual term text */
    int term_id; /* index of term in dictionary */

    bool postings_ready; /* if posting list is available */
    int posting_count; /* #of entries in posting list */
    void *postings; /* actual posting list */
};

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
        int size;
        int n_entries;
    };

    Dictionary();
    ~Dictionary();

    void read(const char *filename);
    int lookup(const char *text, size_t len) const;

    inline PostingListInfo getPostingListInfo(int key, IndexType idxtype) const;
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

int Dictionary::lookup(const char *text, size_t len) const
{
    int h = hasher.hash(text, len);

    const Term *t = buckets[h], *tend = buckets[h+1];
    for(int i=0; t < tend; t++,i++)
    {
        if(len != (size_t)t->text_length)
            continue;
        if(memcmp(text, t->text, len) == 0)
            return buckets[h] - buckets[0] + i;
    }
    return -1;
}

Dictionary::PostingListInfo Dictionary::getPostingListInfo(int key, IndexType idxtype) const
{
    int list_id = idxtype == LEMMATIZED ? terms[key].lemmatized_list_id : key;

    PostingListInfo ret;
    ret.offset = lemmatized[list_id].offset;
    ret.size = lemmatized[list_id+1].offset - ret.offset;
    ret.n_entries = lemmatized[list_id].n_entries;
    return ret;
}

/* -------------------------------------------------------------------------- */

class PostingsReader
{
public:
    struct ReadRq {
        off_t offset;
        int size;
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
    PostingsReader();
    ~PostingsReader();

    void start(const char *positional_filename, const char *lemmatized_filename);
    void stop();

    void request(ReadRq *rqs, int count, IndexType idxtype);
    int wait();
};

void* PostingsReader::runThreadFunc(void *arg) {
    ((PostingsReader*)arg)->threadFunc();
    return NULL;
}

void PostingsReader::threadFunc()
{
    for(;;)
    {
        uint64_t foo;
        eventfd_read(evfd, &foo);

        pthread_mutex_lock(&lock);

        FileIO fio(fd);
        for(int i=0; i<rqs_count; i++) {
            rqs[i].data = fio.read_raw_alloc(rqs[i].size, rqs[i].offset);
            eventfd_write(evfd, 1);
        }
        fd = -1;

        pthread_mutex_unlock(&lock);
    }
}

PostingsReader::PostingsReader() {
    memctx = NULL;
}
PostingsReader::~PostingsReader() {
    if(memctx) stop();
}

int PostingsReader::openIndex(const char *filename, uint32_t magic)
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

void PostingsReader::start(const char *positional_filename, const char *lemmatized_filename)
{
    assert(!memctx);
    memctx = talloc_named_const(NULL, 0, "postings reader");
    assert(memctx);

    evfd = eventfd(0, 0);
    assert(evfd != -1);

    positional_fd = openIndex(positional_filename, 0x50584449);
    lemmatized_fd = openIndex(lemmatized_filename, 0x4c584449);
    fd = -1;

    pthread_mutex_init(&lock, NULL);
    assert(pthread_create(&thread, NULL, &runThreadFunc, this) == 0);
}

void PostingsReader::stop()
{
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    pthread_mutex_destroy(&lock);
    talloc_free(memctx);
}

void PostingsReader::request(ReadRq *rqs, int count, IndexType idxtype)
{
    pthread_mutex_lock(&lock);
    fd = idxtype == POSITIONAL ? positional_fd : lemmatized_fd;
    this->rqs = rqs;
    rqs_count = count;
    eventfd_write(evfd, 1);
    pthread_mutex_unlock(&lock);
}

int PostingsReader::wait(void)
{
    if(fd == -1)
        return 0;

    uint64_t val;
    eventfd_read(evfd, &val);
    return val;
}

/* -------------------------------------------------------------------------- */

/* GLOBALS */
static Artitles artitles;
static Dictionary dictionary;
static PostingsReader posrdr;

/* -------------------------------------------------------------------------- */

static void resolve_terms(QueryNode *node, IndexType idxtype)
{
    if(!node) return;
    if(node->type == QueryNode::TERM)
    {
        node->term_id =
            dictionary.lookup(node->term_text, strlen(node->term_text));

        if(node->term_id == -1)
            node->posting_count = 0;
        else {
            Dictionary::PostingListInfo info =
                dictionary.getPostingListInfo(node->term_id, idxtype);
            node->posting_count = info.n_entries;
        }
    } else {
        resolve_terms(node->lhs, idxtype);
        resolve_terms(node->rhs, idxtype);
    }
}

/* -------------------------------------------------------------------------- */

class TreePostingsReader
{
    void *memctx;

    IndexType idxtype;

    int term_count, done_wridx, done_rdidx;
    QueryNode **terms, **done;

    int rqs_count, rqs_rdidx;
    PostingsReader::ReadRq *rqs;
    int *term_rq_map;

    inline void markAsDone(QueryNode *node);

    static int countLeaves(QueryNode *node);
    static int extractLeaves(QueryNode *node, QueryNode **ptr);
    void killEmptyTerms();
    void createRqs();

public:
    void attach(QueryNode *root, IndexType idxtype, void *memctx);
    QueryNode *wait();
};

void TreePostingsReader::markAsDone(QueryNode *node) {
    node->postings_ready = true;
    done[done_wridx++] = node;
}

void TreePostingsReader::attach(QueryNode *root, IndexType idxtype, void *memctx)
{
    this->idxtype = idxtype;
    this->memctx = memctx;

    done_rdidx = done_wridx = rqs_count = rqs_rdidx = 0;
    term_count = countLeaves(root);
    terms = talloc_array(memctx, QueryNode*, term_count);
    done = talloc_array(memctx, QueryNode*, term_count);
    assert(terms && done);

    extractLeaves(root, terms);
    killEmptyTerms();
    createRqs();
    posrdr.request(rqs, rqs_count, idxtype);
}

QueryNode *TreePostingsReader::wait()
{
    if(done_rdidx < done_wridx)
        return done[done_rdidx++];

    int n = posrdr.wait();
    while(n--)
    {
        PostingsReader::ReadRq *rq = &rqs[rqs_rdidx];
        talloc_steal(memctx, rq->data);

        for(int i=0; i<term_count; i++)
            if(term_rq_map[i] == rqs_rdidx) {
                terms[i]->postings = rq->data;
                markAsDone(terms[i]);
            }

        rqs_rdidx++;
    }

    assert(done_rdidx < done_wridx);
    return done[done_rdidx++];
}

int TreePostingsReader::countLeaves(QueryNode *node)
{
    if(!node) return 0;
    if(node->type == QueryNode::TERM)
        return 1;
    return countLeaves(node->lhs) + countLeaves(node->rhs);
}
int TreePostingsReader::extractLeaves(QueryNode *node, QueryNode **ptr)
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
void TreePostingsReader::killEmptyTerms()
{
    int empty = 0;
    for(int i=0,j=0; i<term_count; i++)
    {
        if(terms[i]->posting_count == 0) {
            markAsDone(terms[i]);
            empty++;
        } else
            terms[j++] = terms[i];
    }
    term_count -= empty;
}
void TreePostingsReader::createRqs()
{
    rqs = talloc_array(memctx, PostingsReader::ReadRq, term_count);
    assert(rqs);

    for(int i=0; i<term_count; i++)
    {
        bool dupli = false;
        for(int j=0; j<i; j++)
            if(terms[j]->term_id == terms[i]->term_id) {
                term_rq_map[i] = term_rq_map[j];
                dupli = true;
                break;
            }
        if(dupli) continue;

        term_rq_map[i] = rqs_count++;
        PostingsReader::ReadRq *rq = &rqs[term_rq_map[i]];

        Dictionary::PostingListInfo info =
            dictionary.getPostingListInfo(terms[i]->term_id, idxtype);
        rq->offset = info.offset;
        rq->size = info.size;
    }
}

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

class QueryEngineBase
{
protected:
    void *memctx;
};

/* -------------------------------------------------------------------------- */

class BooleanQueryEngine : public QueryEngineBase
{
public:
    void run(QueryNode *root);
};

void BooleanQueryEngine::run(QueryNode *root)
{
    memctx = talloc_parent(root);
}

/* -------------------------------------------------------------------------- */

class PhraseQueryEngine : public QueryEngineBase
{
public:
    void run(QueryNode *root);
};

void PhraseQueryEngine::run(QueryNode *root)
{
    memctx = talloc_parent(root);
}

/* -------------------------------------------------------------------------- */

#if 0
static void dump_node(const QueryNode *node, int indent)
{
    printf("%.*s", indent, "                                                                                ");
    if(!node) {
        printf("NULL\n");
        return;
    }
    switch(node->type) {
        case QueryNode::TERM:
            printf("TERM: »%s«\n", node->term);
            return;
        case QueryNode::OR:
            printf("OR\n");
            dump_node(node->lhs, 3+indent);
            dump_node(node->rhs, 3+indent);
            return;
        case QueryNode::AND:
            printf("AND\n");
            dump_node(node->lhs, 3+indent);
            dump_node(node->rhs, 3+indent);
            return;
        case QueryNode::PHRASE:
            printf("PHRASE\n");
            for(QueryNode *p = node->rhs; p; p = p->rhs)
                dump_node(p, 3+indent);
            return;
    }
}
#endif

static void run_query(const char *query)
{
    QueryParser qp;
    QueryNode *root = qp.run(query);
    if(!root)
        return;

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
