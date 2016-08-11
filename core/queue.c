/*
 * queue.c
 *
 *  Created on: Aug 11, 2016
 *      Author: dai
 */

#include "queue.h"
#include <string.h>
#include <stdio.h>

qnode_t * init (
		ssize_t * length)
{
	qnode_t* q = (qnode_t*) calloc (1, sizeof(qnode_t));
	return q;
}


void push (
		qnode_t* head,
		S3TP_PACKET* data)
{
	int i;
	qnode_t *ref = head;
	qnode_t * new, tmp;
	while(1)
	{
		if(ref->next ==0)
		{
			//either we are at the last node;
			break;
		}else if (ref->next->seq > data->hdr->seq)
		{
			break;
			//or our seq number ist lower than the next one, reordering

		}
		ref = ref->next;
	}

	//create a new node
	new = (qnode_t*) calloc(1, sizeof(qnode_t));
	new->seq = data->hdr->seq;

	//insert here
	tmp = ref->next;
	ref->next = new;
	new->next = tmp;
}

S3TP_PACKET* pop (
		qnode_t* head)
{
	qnode_t* tmp;
	S3TP_PACKET* pack;

	//get the lowest seq packet and remove it from queue
	tmp = head->next;
	head->next = head->next->next;
	pack = tmp->payload;

	free(tmp);

	return pack;
}

void deinit(
		qnode_t* head)
{
	qnode_t* ref;
	while(head!=0)
	{
		ref = head;
		head = head->next;
		free(ref);
	}
}
