#include <stdio.h>
#include <stdlib.h>

#include "rtfs_test.h"

#include "uthash/utlist.h"
#include "utils/rtfs_log.h"
#include "utils/declare_utils.h"


/**
 * LL_* → 只需要 next。
 * DL_* → 需要 prev + next。
 * CDL_* → 需要 prev + next（循环双链表）。
 * 所有的数据结构必须自己定义，utlist.h 不会提供结构体。
 */
// typedef struct UtListTestNode
// {
//     int val;

//     struct UtListTestNode *prev;
//     struct UtListTestNode *next;
// } UtListTestNode;
DEFINE_UTLIST_NODE(UtListTestNode, int val)


static UtListTestNode *createNode(int v)
{
    UtListTestNode *node = (UtListTestNode *)malloc(sizeof(struct UtListTestNode));

    node->val = v;
    node->prev = NULL;
    node->next = NULL;


    return node;
}

static void freeNode(UtListTestNode *node) { free(node); }

/**
 * DL_FOREACH_SAFE 的特点是：
 *
 * 在遍历当前节点 node 时，会提前保存 node->next 到 tmp，
 * 因此即使在循环体内删除 node（DL_DELETE）也不会破坏遍历过程。
 *
 * 释放流程：
 * 1. 遍历链表中的每个节点。
 * 2. 使用 DL_DELETE 将节点从链表中摘除。
 * 3. 调用 free 释放节点内存。
 * 4. 最后将 head 置为 NULL，避免悬空指针。
 */
static void freeList(UtListTestNode **head)
{
    UtListTestNode *node = NULL, *tmp = NULL;

    DL_FOREACH_SAFE(*head, node, tmp)
    {
        DL_DELETE(*head, node);

        freeNode(node);
    }

    *head = NULL;
}


RTFS_TEST(UtListAppendTest)
{
    UtListTestNode *head = NULL, *node = NULL;


    // 注意 DL_APPEND 宏里面 node 会被调用多次，所以不能直接给 createNode(1) 这种函数的返回参数，否则每次的结果都不一样!!!
    node = createNode(1);
    DL_APPEND(head, node);

    node = createNode(2);
    DL_APPEND(head, node);

    node = createNode(3);
    DL_APPEND(head, node);

    int i = 1;
    DL_FOREACH(head, node)
    {
        RTFS_LOG(RTFS_LOG_INFO, "%d", node->val);

        TEST_ASSERT_EQUAL(node->val, i);
        ++i;
    }

    TEST_ASSERT_EQUAL(i, 4);


    freeList(&head);
}

RTFS_TEST(UtListPrependTest)
{
    UtListTestNode *head = NULL, *node = NULL;


    node = createNode(1);
    DL_PREPEND(head, node);

    node = createNode(2);
    DL_PREPEND(head, node);

    node = createNode(3);
    DL_PREPEND(head, node);

    int i = 3;
    DL_FOREACH(head, node)
    {
        RTFS_LOG(RTFS_LOG_INFO, "%d", node->val);

        TEST_ASSERT_EQUAL(node->val, i);
        --i;
    }

    TEST_ASSERT_EQUAL(i, 0);


    freeList(&head);
}

RTFS_TEST(UtListDeleteTest)
{
    UtListTestNode *head = NULL;


    UtListTestNode *n1 = createNode(1);
    DL_APPEND(head, n1);

    UtListTestNode *n2 = createNode(2);
    DL_PREPEND(head, n2);

    UtListTestNode *n3 = createNode(3);
    DL_PREPEND(head, n3);

    // 删除 2 号节点。
    DL_DELETE(head, n2);
    freeNode(n2);

    int expect[] = {3, 1};
    int i = 0;
    UtListTestNode *node = NULL;

    DL_FOREACH(head, node)
    {
        RTFS_LOG(RTFS_LOG_INFO, "%d", node->val);

        TEST_ASSERT_EQUAL(node->val, expect[i]);
        ++i;
    }


    freeList(&head);
}

RTFS_TEST(UtListCountTest)
{
    UtListTestNode *head = NULL, *node = NULL;


    node = createNode(1);
    DL_PREPEND(head, node);

    node = createNode(2);
    DL_PREPEND(head, node);

    node = createNode(3);
    DL_PREPEND(head, node);

    int count = 0;

    DL_COUNT(head, node, count);
    TEST_ASSERT_EQUAL(count, 3);


    freeList(&head);
}
