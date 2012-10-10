/*
 * =============================================================================
 *
 *       Filename:  list_sort.h
 *
 *    Description:  list sort.
 *
 *        Created:  08/26/2012 05:59:07 PM
 *
 *         Author:  Fu Haiping (forhappy), haipingf@gmail.com
 *        Company:  ICT ( Institute Of Computing Technology, CAS )
 *
 * =============================================================================
 */

#ifndef UTILITY_LIST_SORT_H
#define UTILITY_LIST_SORT_H


struct list_head;

void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, struct list_head *a,
			  struct list_head *b));

#endif // UTILITY_LIST_SORT_H
