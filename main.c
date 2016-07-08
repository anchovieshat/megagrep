#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ring.h"

typedef struct ThreadWorker {
	pthread_t thread;
} ThreadWorker;

void *check_line(RingBuffer *ring) {
	pthread_cond_wait(&ring->new_data, &ring->mtx);

	GrepPass *g = get_entry(ring);
	if (g == NULL) {
		puts("[NOPE]");
		pthread_mutex_unlock(&ring->mtx);
		pthread_exit(NULL);
	}

	if (g->line != NULL) {
		unsigned long occurances = 0;
		char *fragment;

		fragment = strstr(g->line, g->search_str);
		if (fragment != NULL) {
			printf("[processed] %s", g->line);
			fragment = strtok(g->line, g->search_str);
			while (fragment != NULL) {
				fragment = strtok(NULL, g->search_str);
				occurances++;
			}
		}

		g->ret = occurances;
		pthread_mutex_unlock(&ring->mtx);
		pthread_exit(NULL);
	} else {
		g->ret = 0;
		pthread_mutex_unlock(&ring->mtx);
		pthread_exit(NULL);
	}
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
		puts("not enough arguments!");
		return 1;
	}

	FILE *fp = fopen(argv[1], "r");
	char *line = malloc(256);
	unsigned long accumulated_occurances = 0;

	ThreadWorker *threads = malloc(sizeof(ThreadWorker) * 8);

	RingBuffer ring;
	ring.max_entries = 5;
	ring.head = 0;
	ring.tail = ring.head;
	ring.count = 0;
	ring.buffer = malloc(sizeof(GrepPass *) * ring.max_entries);
	pthread_mutex_init(&ring.mtx, NULL);
	pthread_cond_init(&ring.new_data, NULL);

	GPList *list = NULL;

	int thread_idx = 0;

	for (; thread_idx < 8; thread_idx++) {
		pthread_create(&threads[thread_idx].thread, NULL, (void *)check_line, &ring);
	}

    char *next_line = fgets(line, 256, fp);
	while (next_line != NULL) {
		GrepPass *g = new_gpass(&list, next_line, argv[2]);
		add_entry(&ring, g);
    	next_line = fgets(line, 256, fp);
	}
	pthread_cond_broadcast(&ring.new_data);


	puts("[JOINING]");
	for (int i = 0; i < thread_idx; i++) {
		pthread_join(threads[i].thread, NULL);
	}

	// build hit list, clean up GrepPass objects
    while (list != NULL) {
		accumulated_occurances += list->data->ret;
		GPList *tmp = list->next;
		free(list->data->line);
		free(list->data);
		free(list);
		list = tmp;
	}

	printf("occured %lu times\n", accumulated_occurances);

	pthread_mutex_destroy(&ring.mtx);
	pthread_cond_destroy(&ring.new_data);
	pthread_exit(NULL);
	free(ring.buffer);
	free(threads);
	free(line);
	fclose(fp);
}
