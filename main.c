#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "common.h"

typedef struct Chunk {
	u8 *data;
	u64 size;
	u64 cur_pos;
	u64 idx;
} Chunk;

typedef struct WorkPacket {
	Chunk *chunk;
	char *search_str;
	u64 search_str_len;
	u64 *lookup;
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
	u64 count;
	u32 num_threads;
	u8 done;
} ThreadPool;

typedef struct ThreadContainer {
	ThreadPool *pool;
	u64 thread_idx;
} ThreadContainer;

void *grab_task(ThreadContainer *tc) {
	ThreadPool *pool = tc->pool;
	u64 count = 0;
	for (;;) {
		u64 start_time = rdtsc();
		get_lock(&pool->lock);
		while (pool->count == 0 && !pool->done) {
			wait_for_lock(&pool->not_empty, &pool->lock);
		}
		u64 end_time = rdtsc();

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
		//printf("[THREAD %llu] waited for lock for : %llu\n", tc->thread_idx, end_time - start_time);
		//printf("[THREAD %llu] ran task\n", tc->thread_idx);
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
		ThreadContainer *tc = (ThreadContainer *)malloc(sizeof(ThreadContainer));
		tc->pool = pool;
		tc->thread_idx = i;
		pthread_create(&pool->threads[i], NULL, (void *)grab_task, tc);
	}

	return pool;
}

typedef struct LineRef {
	char *str;
	u64 len;
} LineRef;

LineRef *new_lineref(char *str, u64 len) {
	LineRef *line_ref = (LineRef *)malloc(sizeof(LineRef));
	line_ref->str = str;
	line_ref->len = len;
	return line_ref;
}

LineRef *get_line(Chunk *chunk, u64 max_run) {
	u64 chunk_idx = chunk->idx + 1;
	if (chunk->data[chunk->cur_pos] == 0 || (i8)(chunk->data[chunk->cur_pos]) == EOF || chunk->cur_pos > chunk->size * chunk_idx) {
		return NULL;
	}

	u64 line_idx = 0;
	for (u64 i = chunk->cur_pos; i < chunk->cur_pos + (max_run - 1); i++) {
		char c = chunk->data[i];
		if (c != '\n' && c != EOF) {
			line_idx++;
		} else {
            if (c == '\n') {
				line_idx++;
			}

			LineRef *lr = new_lineref((char *)(chunk->data + chunk->cur_pos), line_idx);
			chunk->cur_pos += line_idx;
			return lr;
		}
	}

	LineRef *lr = new_lineref((char *)(chunk->data + chunk->cur_pos), line_idx);
	chunk->cur_pos += line_idx;
	return lr;
}

u64 bm_strstr(u8 *str, u64 str_len, char *needle, u64 needle_len, u64 *lookup) {
	if (needle_len > str_len) return str_len;
	if (needle_len == 1) {
		u8 *result = (u8 *)memchr(str, *needle, str_len);
		return result ? (result - str) : str_len;
	}

	u64 str_pos = 0;
	while (str_pos <= str_len - needle_len) {
		u8 lookup_char = str[str_pos + needle_len - 1];

		if ((needle[needle_len - 1] == lookup_char) && (memcmp(needle, str+str_pos, needle_len - 1) == 0)) {
			return str_pos;
		}

		str_pos += lookup[lookup_char];
	}

	return str_len;
}

u64 *build_lookup(char *needle, u64 needle_len) {
	u64 *lookup = (u64 *)malloc(sizeof(u64) * 256);
	lookup = memset(lookup, needle_len, sizeof(u64) * 256);

	if (needle_len >= 1) {
		for (u64 i = 0; i < needle_len - 1; ++i) {
			lookup[(u8)needle[i]] = needle_len - 1 - i;
		}
	}

	return lookup;
}

void check_line(WorkPacket *p, LineRef *line) {
	char *tmp = line->str;

	u64 occurances = 0;
	u64 tmp_len = line->len;
	u64 occ_idx = 0;
    while ((occ_idx = bm_strstr((u8 *)tmp, line->len, p->search_str, p->search_str_len, p->lookup)) != line->len) {
		tmp += occ_idx + p->search_str_len;
		tmp_len -= occ_idx + p->search_str_len;
		occurances++;
	}
	if (occurances > 0) {
		//printf("[%llu] %.*s", occurances, (i32)line->len, line->str);
		p->line_no++;
	}

	free(line);
}

void process_chunk(WorkPacket *p) {
	u64 line_no = 1;

    LineRef *next_line = get_line(p->chunk, 256);
	while (next_line != NULL) {
		check_line(p, next_line);
    	next_line = get_line(p->chunk, 256);
		line_no++;
	}
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


void print_chunk(Chunk *c) {
	printf("size: %llu, idx: %llu, cur_pos: %llu, data: %p\n", c->size, c->idx, c->cur_pos, c->data);
}

Chunk *new_chunk(u64 size, u64 chunk_idx, u8 *data) {
	Chunk *c = malloc(sizeof(Chunk));
	c->size = size;
	c->idx = chunk_idx;
	c->cur_pos = (chunk_idx * c->size);
	c->data = data;
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


int main(int argc, char *argv[]) {
    if (argc != 3) {
		puts("incorrect number of arguments! Try ./megagrep search_term file");
		return 1;
	}

	File *file = open_file(argv[2]);
	u64 num_chunks = 16;
	u64 num_threads = 8;

	ThreadPool *pool = new_threadpool(num_threads);
	Chunk **chunks = new_chunks(file, num_chunks);

	u64 start_time = rdtsc();
	for (u64 i = 0; i < num_chunks; i++) {
		WorkPacket *p = malloc(sizeof(WorkPacket));
        p->chunk = chunks[i];
		p->search_str = argv[1];
		p->search_str_len = strlen(p->search_str);
		p->lookup = build_lookup(p->search_str, p->search_str_len);
		p->count = 0;
		add_task(pool, (void *)process_chunk, p);
	}

	finish_work(pool);
	u64 end_time = rdtsc();
	printf("tasks took %llu to run\n", end_time - start_time);

	close_file(file);

	printf("number of chunks: %llu\n", num_chunks);
}
