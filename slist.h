#ifndef _SLIST_H_
#define _SLIST_H_

#define slist_init(list) do { \
	(list)->head = NULL; \
	(list)->tail = &(list)->head; \
} while(0)

#define slist_declare(type, list) struct { type *head, **tail; } list
#define slist_stack_declare(type, list) struct { type *head; } list
#define slist_initializer(list) { .head = NULL, .tail = &(list)->head }
#define slist_stack_initializer(list) { .head = NULL }

#define slist_define(type, list) \
	slist_declare(type, list) = slist_initializer(&list)
#define slist_stack_define(type, list) \
	slist_stack_declare(type, list) = slist_stack_initializer(&list)

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

#endif /* _SLIST_H_ */
