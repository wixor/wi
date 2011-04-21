#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <talloc.h>
#include "bufrw.h"
#include "term.h"

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

    QueryNode *lhs, *rhs;
    int deps; /* unmet dependencies for the node */

    const char *term_text; /* actual term text */
    int term_id; /* index of term in dictionary */
    uint32_t posting_offs; /* posting list offset in index */

    int posting_count; /* #of entries in posting list */
    int *postings; /* actual posting list */
};

/* -------------------------------------------------------------------------- */

class Artitles
{
    void *memctx;
    char **titles;
public:
    ~Artitles();
    void read(const char *filename);
    inline const char *lookup(int key) const { return titles[key]; }
};

Artitles::~Artitles() {
    if(memctx)
        talloc_free(memctx);
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
    printf("article titles read in %.03lf seconds.\n", timer.end());
}

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
        int length;
        int n_entries;
    };

    ~Dictionary();

    void read(const char *filename);
    int lookup(const char *text, size_t len) const;

    inline PostingListInfo getLemmatizedIndexInfo(int key) const;
    inline PostingListInfo getPositionalIndexInfo(int key) const;
};

Dictionary::~Dictionary() {
    if(memctx)
        talloc_free(memctx);
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
    printf("dictionary read in %.03lf seconds\n", timer.end());
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

Dictionary::PostingListInfo Dictionary::getLemmatizedIndexInfo(int key) const
{
    int list_id = terms[key].lemmatized_list_id;

    PostingListInfo ret;
    ret.offset = lemmatized[list_id].offset;
    ret.length = lemmatized[list_id+1].offset - ret.offset;
    ret.n_entries = lemmatized[list_id].n_entries;
    return ret;
}

Dictionary::PostingListInfo Dictionary::getPositionalIndexInfo(int key) const
{
    PostingListInfo ret;
    ret.offset = positional[key].offset;
    ret.length = positional[key+1].offset - ret.offset;
    ret.n_entries = positional[key].n_entries;
    return ret;
}

/* -------------------------------------------------------------------------- */

class PostingsReader
{
    struct rq {
        off_t offset;
        off_t length;
        void **dest;
    };

    int evfd;
    pthread_t thread;
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

public:
    QueryNode *run(const char *query);
};

QueryNode *QueryParser::makeNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs, const char *term) const
{
    QueryNode *node = talloc_zero(memctx, QueryNode);
    assert(node);
    node->type = type;
    node->rhs = rhs;
    node->lhs = lhs;
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
        fprintf(stderr, "malformed query: %s\n", err);
    }

    talloc_free(memctx);
    return NULL;
}

/* -------------------------------------------------------------------------- */

class QueryExecution
{
    void *memctx;

};
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

static void process_query()
{
    static char buffer[1024];

    printf("Enter query: "); fflush(stdout);
    fgets(buffer, 1023, stdin);

    QueryNode *root = parse_query(buffer);
    dump_node(root, 0);
    if(root) talloc_free(root);
}
#endif

int main(void)
{
    return 0;
}