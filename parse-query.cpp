#include <talloc.h>
#include "bufrw.h"
#include "parse-query.h"

bool TermReader::isWhitespace(int c)
{
    static const char stopchars[] =
        " \n\t\r`~!@#$%^&*-_=+\[]{};':,./<>?";
    for(const char *p = stopchars; *p; p++)
        if(c == *p)
            return true;
    return false;
}
bool TermReader::isTermSeparator(int c) {
    return isWhitespace(c) || c == '|' || c == ')' || c == '(' || c == '"';
}

bool TermReader::eatSymbol(int sym)
{
    size_t start = tell();

    while(!eof())
    {
        int c = read_utf8();
        if(c == sym)
            return true;
        if(!isWhitespace(c))
            break;
    }

    seek(start);
    return false;
}

int TermReader::eatWhitespace()
{
    int count = 0;
    while(!eof()) {
        size_t where = tell();
        int c = read_utf8();
        if(!isWhitespace(c)) {
            seek(where);
            break;
        }
        count++;
    }
    return count;
}

char *TermReader::readTerm(void *memctx)
{
    eatWhitespace();

    size_t start = tell(), end = start;
    while(!eof()) {
        end = tell();
        int c = read_utf8();
        if(isTermSeparator(c))
            break;
    }
    seek(end);

    if(start == end)
        return NULL;

    size_t len = end-start;
    char *term = (char *)talloc_size(memctx, len+1);
    assert(term);

    rd.read_raw_at(start, term, len);
    term[len] = '\0';

    return term;
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
    QueryNode *node = talloc(memctx, QueryNode);
    assert(node);
    node->type = type;
    node->rhs = rhs;
    node->lhs = lhs;
    node->term = term;
    if(rhs) talloc_steal(node, rhs);
    if(lhs) talloc_steal(node, lhs);
    if(term) talloc_steal(node, term);
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
    char *term = trd.readTerm(memctx);
    return !term ? NULL : makeNode(QueryNode::TERM, NULL, NULL, term);
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
        char *tstr = trd.readTerm(memctx);
        if(!tstr) break;

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
    QueryNode *root = NULL;

    try {
        root = parseQuery();
    } catch(const char *err) {
        fprintf(stderr, "malformed query: %s\n", err);
    }

    if(root) {
        talloc_steal(NULL, root);
        assert(talloc_total_size(memctx) == 0);
    }
    talloc_free(memctx);
    return root;
}

struct QueryNode* parse_query(const char *query)
{
    QueryParser qp;
    return qp.run(query);
}
