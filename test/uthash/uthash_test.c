#include <stdio.h>
#include <stdlib.h>

#include "rtfs_test.h"

#include "uthash/uthash.h"


typedef struct UtHashTestEntry
{
    int id;
    int cookie;
    UT_hash_handle hh;
} UtHashTestEntry;


RTFS_TEST(UtHashTest)
{
    int i;
    UtHashTestEntry *user, *users = NULL;

    /* create elements */
    for (i = 0; i < 10; ++i)
    {
        user = (UtHashTestEntry *)malloc(sizeof(UtHashTestEntry));
        if (user == NULL)
        {
            exit(-1);
        }
        user->id = i;
        user->cookie = i * i;
        HASH_ADD_INT(users, id, user);
    }

    for (user = users; user != NULL; user = (UtHashTestEntry *)(user->hh.next))
    {
        printf("user %d, cookie %d\n", user->id, user->cookie);
    }
}
