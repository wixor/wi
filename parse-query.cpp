#include <talloc.h>
#include "bufrw.h"
#include "parse-query.h"

class QueryParser
{
    void *memctx;
    Reader rd;

    QueryNode *makeNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs, const char *term)
    {
        QueryNode *node = talloc(memctx, QueryNode);
        assert(node);
        if(memctx == NULL)
            memctx = node;
        node->type = type;
        node->rhs = rhs;
        node->lhs = lhs;
        node->term = term;
        return node;
    }
    QueryNode *makeOpNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs)
    {
        return makeNode(type, lhs, rhs, NULL);
    }
    QueryNode *makeTermNode(size_t offs, size_t len)
    {
        QueryNode *node = makeNode(QueryNode::TERM, NULL, NULL, NULL);

        char *termcpy = (char *)talloc_size(node, len+1);
        assert(termcpy);
        rd.read_raw_at(offs, termcpy, len);
        termcpy[len] = '\0';
        node->term = termcpy;

        return node;
    }

    bool eatSymbol(int sym)
    {
        size_t start = rd.tell();

        while(!rd.eof())
        {
            int c = rd.read_utf8();
            if(c == sym)
                return true;
            if(c != ' ' && c != '\t')
                break;
        }

        rd.seek(start);
        return false;
    }

    QueryNode *parse0() /* parentheses + terms */
    {
        if(eatSymbol('(')) {
            QueryNode *inside = parse2();
            if(!eatSymbol(')'))
                throw "malformed query: unmatched parentheses";
            return inside;
        }

        eatSymbol(' ');

        size_t start = rd.tell(), end = start;
        while(!rd.eof()) {
            end = rd.tell();
            int c = rd.read_utf8();
            if(c == ' ' || c == '\t' || c == '|' || c == ')' || c == '(')
                break;
        }
        rd.seek(end);

        return start == end ? NULL : makeTermNode(start, end-start);
    }

    QueryNode *parse1() /* or-s */
    {
        QueryNode *lhs = parse0();
        if(!eatSymbol('|'))
            return lhs;
        QueryNode *rhs = parse1();
        if(!rhs)
            throw "malformed query: no rhs for '|'";
        return makeOpNode(QueryNode::OR, lhs, rhs);
    }

    QueryNode *parse2() /* and-s */
    {
        QueryNode *lhs = parse1();
        if(!eatSymbol(' '))
            return lhs;
        QueryNode *rhs = parse2();
        if(!rhs)
            return lhs;
        return makeOpNode(QueryNode::AND, lhs, rhs);
    }

public:
    QueryNode *run(const char *query)
    {
        memctx = NULL;
        rd.attach(query, strlen(query));
        try {
            return parse2();
        } catch(const char *err) {
            fputs(err, stderr);
        }
        if(memctx)
            talloc_free(memctx);
        return NULL;
    }
};

struct QueryNode* parse_query(const char *query)
{
    QueryParser qp;
    return qp.run(query);
}
