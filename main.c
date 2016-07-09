#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

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
		//puts("[GRAB] LOCK TO CHECK COUNT/DONE");
		pthread_mutex_lock(&pool->lock);
		while (pool->count == 0 && !pool->done) {
			//puts("[GRAB] WAITING FOR NOT EMPTY");
			pthread_cond_wait(&pool->not_empty, &pool->lock);
		}

		//puts("[GRAB] GOT NOT EMPTY AND LOCK");
		if (pool->done && pool->count == 0) {
			break;
		}

		Task *tmp;
		if (pool->count > 0) {
			tmp = pool->front;
			pool->front = pool->front->next;
			pool->count--;
			pthread_mutex_unlock(&pool->lock);
			//puts("[GRAB] GOT WORK, RELEASED LOCK");
		} else {
			//puts("[GRAB] EMPTY QUEUE");
			pthread_mutex_unlock(&pool->lock);
			//puts("[GRAB] RELEASED LOCK, EXITING");

			ThreadReturn *r = malloc(sizeof(ThreadReturn));
			r->count = count;
			pthread_exit(r);
		}

		//puts("[GRAB] RUNNING WORK");
		tmp->task(tmp->args);
		count += ((WorkPacket *)tmp->args)->count;
		free(tmp);
	}

	pthread_mutex_unlock(&pool->lock);
	//puts("[GRAB] RELEASED LOCK, EXITING");
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
		printf("[%lu] %s", p->line_no, p->line);
	}

	free(p->line);
	p->count = occurances;
}

void add_task(ThreadPool *pool, void *(*task)(void *), void *args) {
	//puts("[ADD] GETTING LOCK");
	pthread_mutex_lock(&pool->lock);
	//puts("[ADD] GOT LOCK");
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

	//puts("[ADD] RELEASING LOCK");
	pthread_mutex_unlock(&pool->lock);

	//puts("[ADD] SIGNALING NOT EMPTY");
	pthread_cond_signal(&pool->not_empty);
}

u64 finish_work(ThreadPool *pool) {
	//puts("[DESTROY] BROADCASTING AND JOINING");
	u64 hitcount = 0;
	pthread_mutex_lock(&pool->lock);
	pool->done = 1;
    for (u32 i = 0; i < pool->num_threads; i++) {
		pthread_mutex_unlock(&pool->lock);
		pthread_cond_broadcast(&pool->not_empty);
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

int main(int argc, char *argv[]) {
    if (argc != 3) {
		puts("not enough arguments!");
		return 1;
	}

	FILE *fp = fopen(argv[2], "r");
	char *line = malloc(256);

	ThreadPool *pool = new_threadpool(1);

	u64 line_no = 1;
    char *next_line = fgets(line, 256, fp);
	while (next_line != NULL) {
		WorkPacket *p = malloc(sizeof(WorkPacket));

		u64 sz = strlen(next_line);
		p->line = malloc(sz+1);
		strncpy(p->line, next_line, sz);
		p->line[sz] = '\0';

		p->line_no = line_no;
		p->search_str = argv[1];
		p->count = 0;

		add_task(pool, (void *)check_line, p);
    	next_line = fgets(line, 256, fp);
		line_no++;
	}

	u64 hitcount = finish_work(pool);
	printf("%lu\n", hitcount);
}
