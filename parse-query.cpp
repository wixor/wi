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
        node->type = type;
        node->rhs = rhs;
        node->lhs = lhs;
        node->term = term;
        if(rhs) talloc_steal(node, rhs);
        if(lhs) talloc_steal(node, lhs);
        if(term) talloc_steal(node, term);
        return node;
    }
    QueryNode *makeOpNode(QueryNode::Type type, QueryNode *lhs, QueryNode *rhs)
    {
        return makeNode(type, lhs, rhs, NULL);
    }
    QueryNode *makeTermNode(size_t offs, size_t len)
    {
        char *termcpy = (char *)talloc_size(memctx, len+1);
        assert(termcpy);
        rd.read_raw_at(offs, termcpy, len);
        termcpy[len] = '\0';
        return makeNode(QueryNode::TERM, NULL, NULL, termcpy);
    }

    static inline bool isWhitespace(int c) {
        return c == ' ' || c == '\n' || c == '\t';
    }
    static inline bool isTermSeparator(int c) {
        return  isWhitespace(c) || c == '|' || c == ')' || c == '(' || c == '"';
    }
    bool eatSymbol(int sym)
    {
        size_t start = rd.tell();

        while(!rd.eof())
        {
            int c = rd.read_utf8();
            if(c == sym)
                return true;
            if(!isWhitespace(c))
                break;
        }

        rd.seek(start);
        return false;
    }
    void eatWhitespace()
    {
        while(!rd.eof()) {
            size_t where = rd.tell();
            int c = rd.read_utf8();
            if(!isWhitespace(c)) {
                rd.seek(where);
                break;
            }
        }
    }

    QueryNode *parseTerm()
    {
        eatWhitespace();

        size_t start = rd.tell(), end = start;
        while(!rd.eof()) {
            end = rd.tell();
            int c = rd.read_utf8();
            if(isTermSeparator(c))
                break;
        }
        rd.seek(end);

        return start == end ? NULL : makeTermNode(start, end-start);
    }

    QueryNode *parse0() /* parentheses + terms */
    {
        if(eatSymbol('(')) {
            QueryNode *inside = parse2();
            if(!eatSymbol(')'))
                throw "unmatched parentheses";
            return inside;
        }
        return parseTerm();
    }

    QueryNode *parse1() /* or-s */
    {
        QueryNode *lhs = parse0();
        if(!eatSymbol('|'))
            return lhs;
        QueryNode *rhs = parse1();
        if(!rhs)
            throw "no rhs for '|'";
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

    QueryNode *parsePhrase() /* "query" */
    {
        if(!eatSymbol('"'))
            return NULL;

        QueryNode *root = makeOpNode(QueryNode::PHRASE, NULL, NULL);

        QueryNode *last = root;
        while(!rd.eof()) {
            QueryNode *term = parseTerm();
            if(!term) break;
            talloc_steal(last, term);
            last->rhs = term;
            last = term;
        }

        if(!eatSymbol('"'))
            throw "unmatched quotes";

        return root;
    }

    QueryNode *parseQuery()
    {
        QueryNode *root = NULL;
        if(!root) root = parsePhrase();
        if(!root) root = parse2();
        eatWhitespace();

        if(rd.eof()) return root;
        throw "garbage at end";
    }

public:
    QueryNode *run(const char *query)
    {
        memctx = talloc_new(NULL);
        rd.attach(query, strlen(query));
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
};

struct QueryNode* parse_query(const char *query)
{
    QueryParser qp;
    return qp.run(query);
}
