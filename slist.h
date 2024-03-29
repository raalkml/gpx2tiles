#ifndef _SLIST_H_
#define _SLIST_H_

#define slist_init(list) do { \
	(list)->head = NULL; \
	(list)->tail = &(list)->head; \
} while(0)

#define SLIST_DECLARE(type, list) struct { type *head, **tail; } list
#define SLIST_STACK_DECLARE(type, list) struct { type *head; } list
#define SLIST_INITIALIZER(list) { .head = NULL, .tail = &(list)->head }
#define SLIST_STACK_INITIALIZER(list) { .head = NULL }

#define SLIST_DEFINE(type, list) \
	SLIST_DECLARE(type, list) = SLIST_INITIALIZER(&list)
#define SLIST_STACK_DEFINE(type, list) \
	SLIST_STACK_DECLARE(type, list) = SLIST_STACK_INITIALIZER(&list)

#define slist_empty(list) (!(list)->head)

#define slist_append(list, i) do { \
	typeof(*(list)->head) **__tail = (list)->tail; \
	(i)->next = NULL; \
	(list)->tail = &(i)->next; \
	*__tail = i; \
} while(0)

#define slist_push(list, i) do { \
	(i)->next = (list)->head; \
	(list)->head = (i); \
} while(0)

#define slist_pop(list) ({ \
	typeof(*(list)->head) *__head = (list)->head; \
	(list)->head = __head->next; \
	if (!__head->next) \
		(list)->tail = &(list)->head; \
	__head; \
})

#define slist_stack_pop(list) ({ \
	typeof(*(list)->head) *__head = (list)->head; \
	(list)->head = __head->next; \
	__head; \
})

#define slist_for_each(i, list) \
	for (i = (list)->head; i; i = i->next)

#endif /* _SLIST_H_ */
