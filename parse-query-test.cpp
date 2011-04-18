#include <stdio.h>
#include <talloc.h>
#include "parse-query.h"

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

int main(void)
{
    for(;;)
        process_query();
    return 0;
}
