#ifndef RING_H
#define RING_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct RingBuffer;

typedef struct GrepPass {
	char *line;
	char *search_str;
	struct RingBuffer *ring;
	unsigned long ret;
} GrepPass;

typedef struct GPList {
	GrepPass *data;
	struct GPList *next;
} GPList;

typedef struct RingBuffer {
	GrepPass **buffer;
	pthread_mutex_t mtx;
	pthread_cond_t new_data;
	int head;
	int tail;
	int count;
	int max_entries;
} RingBuffer;

void print_greppass(GrepPass *g) {
	if (g != NULL) {
		printf("[PRINT] %s[PRINT-2] %s, %lu\n", g->line, g->search_str, g->ret);
	} else {
		puts("entry is null!");
	}
}

GrepPass *new_gpass(GPList **head, char *line, char *search_str) {
	GrepPass *g = malloc(sizeof(GrepPass));
	g->line = malloc(256);
	strncpy(g->line, line, strlen(line));
	g->search_str = search_str;
	g->ret = 0;

	GPList *tmp = malloc(sizeof(GPList));
	tmp->data = g;
	tmp->next = *head;
	*head = tmp;
	return g;
}

int add_entry(RingBuffer *ring, GrepPass *g) {
	pthread_mutex_lock(&ring->mtx);
	if (ring->count < ring->max_entries) {
		ring->buffer[ring->head] = g;
		ring->head = (ring->head % ring->max_entries) + 1;
		ring->count++;

		pthread_mutex_unlock(&ring->mtx);
		pthread_cond_signal(&ring->new_data);
		printf("[SIG] ring->count: %d\n", ring->count);
		return 0;
	} else {
		//puts("maxed");
	}
	pthread_mutex_unlock(&ring->mtx);
	return -1;
}

void print_ring(RingBuffer *ring) {
	for (int i = 0; i < ring->count; i++) {
		printf("%d ", i);
		print_greppass(ring->buffer[i]);
	}
}

GrepPass *get_entry(RingBuffer *ring) {
	GrepPass *g = NULL;

	if (ring->count > 0) {
		g = ring->buffer[ring->tail];
		printf("[GET] %s\n", g->line);
		ring->tail = (ring->tail % ring->max_entries) + 1;
		ring->count--;
	}

	return g;
}

#endif
