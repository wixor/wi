#include <talloc.h>
#include "bufrw.h"
#include "term.h"

/* -------------------------------------------------------------------------- */

struct QueryNode
{
    enum { AND, OR, PHRASE, TERM };
    QueryNode *lhs, *rhs;

    const char *term_text;
    int term_id;
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
    QueryNode *run(const void *mc2, const char *query);
};

QueryNode *QueryParser::makeNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs, const char *term) const
{
    QueryNode *node = talloc_zero(memctx, QueryNode);
    assert(node);
    node->type = type;
    node->rhs = rhs;
    node->lhs = lhs;
    node->term = term;
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

QueryNode *QueryParser::run(const void *mc2, const char *query)
{
    memctx = talloc_new(mc2);
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

int main(void)
{
    return 0;
}