#ifndef __PARSE_QUERY_H__
#define __PARSE_QUERY_H__

struct QueryNode {
    enum Type { AND, OR, PHRASE, TERM } type;
    QueryNode *rhs, *lhs;
    const char *term;
};

QueryNode* parse_query(const char *query);

#endif
