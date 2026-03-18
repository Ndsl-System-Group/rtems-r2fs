#ifndef _DECLARE_UTILS_H_
#define _DECLARE_UTILS_H_


#define DEFINE_UTLIST_NODE(type, fields) \
    typedef struct type                  \
    {                                    \
        fields;                          \
        struct type *prev;               \
        struct type *next;               \
    } type;


#endif
