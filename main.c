#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "common.h"

typedef struct WorkPacket {
	char *line;
	char *search_str;
	u64 count;
	u64 line_no;
} WorkPacket;

typedef struct ThreadReturn {
	u64 count;
} ThreadReturn;

typedef struct Task {
	void *(*task)(void *);
	void *args;
	struct Task *next;
} Task;

typedef struct ThreadPool {
	Task *front;
	Task *rear;
	pthread_t *threads;
	pthread_cond_t not_empty;
	pthread_mutex_t lock;
	u32 count;
	u32 num_threads;
	u8 done;
} ThreadPool;

void *grab_task(ThreadPool *pool) {
	u64 count = 0;
	for (;;) {
		get_lock(&pool->lock);
		while (pool->count == 0 && !pool->done) {
			wait_for_lock(&pool->not_empty, &pool->lock);
		}

		if (pool->done && pool->count == 0) {
			break;
		}

		Task *tmp;
		if (pool->count > 0) {
			tmp = pool->front;
			pool->front = pool->front->next;
			pool->count--;
			release_lock(&pool->lock);
		} else {
			release_lock(&pool->lock);

			ThreadReturn *r = malloc(sizeof(ThreadReturn));
			r->count = count;
			pthread_exit(r);
		}

		tmp->task(tmp->args);
		count += ((WorkPacket *)tmp->args)->count;
		free(tmp);
	}

	release_lock(&pool->lock);
	ThreadReturn *r = malloc(sizeof(ThreadReturn));
	r->count = count;
	pthread_exit(r);
}

ThreadPool *new_threadpool(u32 num_of_threads) {
	ThreadPool *pool = malloc(sizeof(ThreadPool));
	pthread_cond_init(&pool->not_empty, NULL);
	pthread_mutex_init(&pool->lock, NULL);
	pool->front = NULL;
	pool->rear = NULL;
	pool->count = 0;
	pool->done = 0;
	pool->threads = malloc(sizeof(pthread_t) * num_of_threads);
	pool->num_threads = num_of_threads;

	for (u32 i = 0; i < num_of_threads; i++) {
		pthread_create(&pool->threads[i], NULL, (void *)grab_task, pool);
	}

	return pool;
}

void check_line(WorkPacket *p) {
	u64 occurances = 0;
	char *tmp = p->line;

 	while ((tmp = strstr(tmp, p->search_str)) != NULL) {
		occurances++;
		tmp += strlen(p->search_str);
	}

	if (occurances > 0) {
		printf("[%llu] %s", p->line_no, p->line);
	}

	free(p->line);
	p->count = occurances;
}

void add_task(ThreadPool *pool, void *(*task)(void *), void *args) {
	get_lock(&pool->lock);
    Task *tmp = malloc(sizeof(Task));
	tmp->args = args;
	tmp->task = task;

	if (pool->count == 0) {
		pool->front = tmp;
		pool->rear = tmp;
		pool->count += 1;
	} else {
		pool->rear->next = tmp;
		pool->rear = tmp;
		pool->count += 1;
	}

	release_lock(&pool->lock);
	signal_to_locks(&pool->not_empty);
}

u64 finish_work(ThreadPool *pool) {
	u64 hitcount = 0;
	get_lock(&pool->lock);
	pool->done = 1;
    for (u32 i = 0; i < pool->num_threads; i++) {
		release_lock(&pool->lock);
		broadcast_to_locks(&pool->not_empty);
		ThreadReturn *r = NULL;
		pthread_join(pool->threads[i], (void *)&r);
		hitcount += r->count;
		free(r);
	}

	free(pool->threads);
	pthread_mutex_destroy(&pool->lock);
	pthread_cond_destroy(&pool->not_empty);

	return hitcount;
}

typedef struct File {
	u8 *data;
	u64 size;
} File;

typedef struct Chunk {
	u8 *data;
	u64 size;
	u64 cur_pos;
	u64 idx;
} Chunk;

Chunk *new_chunk(u64 size, u64 chunk_idx, u8 *data) {
	Chunk *c = malloc(sizeof(Chunk));
	c->size = size;
	c->idx = chunk_idx + 1;
	c->cur_pos = (chunk_idx * c->size);
	c->data = c->cur_pos + data;
	return c;
}

Chunk **new_chunks(File *f, u64 num_chunks) {
	Chunk **chunks = (Chunk **)malloc(sizeof(Chunk *) * num_chunks);

	for (u64 i = 0; i < num_chunks; i++) {
		chunks[i] = new_chunk(f->size / num_chunks, i, f->data);
	}

	return chunks;
}

File *open_file(char *filename) {
	File *f = (File *)malloc(sizeof(File));
	struct stat file_state;

	i32 fd = open(filename, O_RDONLY);
	fstat(fd, &file_state);

	u8 *data = mmap(0, file_state.st_size, PROT_READ, MAP_SHARED, fd, 0);
    f->data = data;
	f->size = file_state.st_size;
	return f;
}

void close_file(File *f) {
	munmap(f->data, f->size);
	free(f);
}

char *get_line(Chunk *chunk, u64 max_run) {
	if (chunk->data[chunk->cur_pos] == 0 || (i8)(chunk->data[chunk->cur_pos]) == EOF || chunk->cur_pos > chunk->size * chunk->idx) {
		printf("%s\n", BOOL_FMT(chunk->cur_pos > chunk->size * chunk->idx));
		return NULL;
	}

	char *line = malloc(max_run);

	u64 line_idx = 0;
	for (u64 i = chunk->cur_pos; i < chunk->cur_pos + (max_run - 1); i++) {
		char c = chunk->data[i];
		if (c != '\n' && c != EOF) {
			line[line_idx] = c;
			line_idx++;
		} else {
            if (c == '\n') {
				line[line_idx] = '\n';
				line_idx++;
			} else if (c == EOF) {
				printf("EOF");
			}

			line[line_idx] = 0;
			chunk->cur_pos += line_idx;
			return line;
		}
	}

	line[line_idx] = 0;
	chunk->cur_pos += line_idx;
	return line;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
		puts("incorrect number of arguments! Try ./megagrep search_term file");
		return 1;
	}

	File *file = open_file(argv[2]);
	ThreadPool *pool = new_threadpool(8);
	Chunk **chunks = new_chunks(file, 2);

	u64 line_no = 1;
    char *next_line = get_line(chunks[0], 256);
	while (next_line != NULL) {
		WorkPacket *p = malloc(sizeof(WorkPacket));

		p->line = strdup(next_line);
		p->line_no = line_no;
		p->search_str = argv[1];
		p->count = 0;

		add_task(pool, (void *)check_line, p);
    	next_line = get_line(chunks[0], 256);
		line_no++;
	}

	u64 hitcount = finish_work(pool);
	printf("%llu\n", hitcount);
	close_file(file);
}
