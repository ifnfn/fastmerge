#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <values.h>
#include <string.h>
#include <time.h>
#include <sys/dir.h>
#include <pthread.h>
#include <semaphore.h>

#include "btree.h"
#include "store.h"
#include "ui.h"
#include "info.h"
#include "list.h"

#define MAX_THREAD 2
#define MAX_STR 128
#define MAX_BUF 1000

struct bthread_info;

struct info_node {
	struct list_head head;
	char str[MAX_STR];
};

struct mqueue {
	sem_t             get_sem;
	pthread_mutex_t   lock;
	struct info_node **node;
	int front, rear, count;
};

struct mqueue *mq_create(int size, int num)
{
	struct mqueue* mq;
	int i;

	if (num > size)
		num = size;
	mq = (struct mqueue *)malloc(sizeof(struct mqueue));
	memset(mq, 0, sizeof(struct mqueue));
	mq->count = size;
	mq->node = (struct info_node **)calloc(size, sizeof(struct info_node *));

	pthread_mutex_init(&mq->lock, NULL);

	mq->front = mq->rear = 0;
	sem_init(&mq->get_sem, 0, num);
	for (i = 0; i < num; i++) {
		mq->node[i] = (struct info_node *)malloc(sizeof(struct info_node));
	}

	mq->front = 0;
	mq->rear = num;

	return mq;
}

void mq_free(struct mqueue *mq)
{
	int i;
	for (i = 0; i < mq->count; i++) {
		if (mq->node[i])
			free(mq->node[i]);
	}
	
	sem_destroy(&mq->get_sem);
	pthread_mutex_destroy(&mq->lock);
	free(mq);
}

void mq_append(struct mqueue *mq, struct info_node *node, int lock)
{
	int a = (mq->rear + 1) % mq->count;
	if (a == mq->front) {
		printf("a=%d, mq->rear=%d, mq->rear + 1 = %d, mq->rear + 1 % mq->count = %d\n", a, mq->rear, mq->rear + 1, (mq->rear + 1) % mq->count);
		printf("......... is FULL\n");
	}

	if (lock)
		pthread_mutex_lock(&mq->lock);

	mq->node[mq->rear] = node;
	mq->rear = (mq->rear + 1) % mq->count;

	if (lock)
		pthread_mutex_unlock(&mq->lock);

	sem_post(&mq->get_sem);
}

struct info_node *mq_get(struct mqueue *mq, int lock)
{
	struct info_node *node = NULL;

	sem_wait(&mq->get_sem);

	if (lock)
		pthread_mutex_lock(&mq->lock);

	if (mq->front != mq->rear) {
		node = mq->node[mq->front];
		mq->node[mq->front] = NULL;
		mq->front = (mq->front + 1) % mq->count;
	}
	if (lock)
		pthread_mutex_unlock(&mq->lock);

	return node;
}

struct bthread_node {
	int id;
	int count;
	struct list_head info_str_head;
	struct mqueue *info_queue;
	struct bthread_info *bi;
	int add, update;

	struct btree *tree;
	struct store *store;
	pthread_mutex_t mutex;
	sem_t sem;
	pthread_t thread;
	int eof;
};

#define NODE_LOCK(node)   pthread_mutex_lock(&((node)->mutex))
#define NODE_UNLOCK(node) pthread_mutex_unlock((&(node)->mutex))

struct bthread_info {
	struct bthread_node node[MAX_THREAD];
	struct mqueue *free_queue;
};

struct info_node *node_list_get(struct list_head *head)
{
	struct info_node *sta = NULL;

	if (!list_empty(head)) {
		struct list_head *pos, *n;
		list_for_each_safe(pos, n, head) {
			sta = list_entry(pos, struct info_node, head);
			list_del(pos);
			break;
		}
	}

	return sta;
}

static int userinfo_insert(struct btree *tree, char *info_str, int *add, int *update)
{
	struct user_info new_data;
	char key[20] = {0, }, *p;
	int len;

	if (tree == NULL || info_str == NULL)
		return -1;

	p = strchr(info_str, ',');

	len  =p - info_str;
	if (len > 18)
		len = 18;

	memcpy(key, info_str, p - info_str);

	memset(&new_data, 0, sizeof(struct user_info));
	userinfo_parser(&new_data, info_str);

	btree_insert(tree, &new_data, key, add, update);

	return 0;
}

void *insert_thread(struct bthread_node *thread_node)
{
	struct info_node *str_node;

	thread_node->add = thread_node->update = 1;

	while (!thread_node->eof) {
		str_node = mq_get(thread_node->info_queue, 1);
//		sem_wait(&thread_node->sem);
//		NODE_LOCK(thread_node);
//		str_node = node_list_get(&thread_node->info_str_head);
//		NODE_UNLOCK(thread_node);

		if (str_node) {
			thread_node->count++;
			userinfo_insert(thread_node->tree, str_node->str, &thread_node->add, &thread_node->update);
			mq_append(thread_node->bi->free_queue, str_node, 1);
		}
		else  {
			break;
		}
	} 

	printf("insert %d pthread exit\n", thread_node->count);
	pthread_exit(0);

	return NULL;
}

static struct bthread_info *btree_thread_ui_create(void)
{
	int i;
	struct bthread_info *bi = (struct bthread_info*)calloc(1, sizeof(struct bthread_info));

	bi->free_queue = mq_create(MAX_BUF, MAX_BUF - 2);

	for (i = 0; i < MAX_THREAD; i++) {
		bi->node[i].id    = i;
		bi->node[i].count = 0;
		bi->node[i].eof   = 0;
		bi->node[i].bi    = bi; 
		bi->node[i].info_queue = mq_create(MAX_BUF + 2, 0);
		bi->node[i].store = store_open_memory(sizeof(struct user_info), 102400);
		bi->node[i].tree  = btree_new_memory(bi->node[i].store, \
						(int(*)(const void *, const void *))userinfo_compare, (int (*)(void*, void*))userinfo_update);

		pthread_mutex_init(&bi->node[i].mutex, NULL);
		INIT_LIST_HEAD    (&bi->node[i].info_str_head);
		sem_init          (&bi->node[i].sem, 0, 0);

		pthread_create(&bi->node[i].thread, NULL, (void *(*)(void*))insert_thread, bi->node + i);
	}

	return bi;
}

int btree_thread_ui_addfile_fopen(struct bthread_info *bi, const char *filename, int *add, int *update)
{
	if (filename && bi) {
		FILE *fp;

		if ((fp  = fopen(filename, "r")) != NULL) {
			while (!feof(fp)) {
				struct info_node *node = mq_get(bi->free_queue, 0);

				if (fgets(node->str, sizeof(node->str) - 1, fp)) {
					int mon;

					if (strlen(node->str) < 8) {
						mq_append(bi->free_queue, node, 1);
						continue;
					}

					mon = FAST_HASH(node->str) % MAX_THREAD;
					mq_append(bi->node[mon].info_queue, node, 1);
//					NODE_LOCK(&bi->node[mon]);
//					list_add(&node->head, &bi->node[mon].info_str_head);
//					NODE_UNLOCK(&bi->node[mon]);
//					sem_post(&bi->node[mon].sem);
				}
				else 
					mq_append(bi->free_queue, node, 1);
			}
			fclose(fp);

			return 0;
		}
	}

	return -1;
}

int btree_thread_ui_addfile_open(struct bthread_info *bi, const char *filename, int *add, int *update)
{
	if (filename && bi) {
		int fd, len, idx = 0;
#define BUFMAX 128
		char buffer[BUFMAX];

		fd = open(filename, O_RDONLY);
		if (fd < 0)
			return -1;

		while (1) {
			char *p;
			len = read(fd, buffer + idx, BUFMAX - idx);
			if (len <= 0)
				break;
			p = buffer;
			while (idx < len) {
				char *x = strchr(p, '\n');
				if (x) {
					*x = 0;
					int mon;
					struct info_node *node = mq_get(bi->free_queue, 0);

					strncpy(node->str, p, sizeof(node->str) - 1);

					if (strlen(node->str) < 8) {
						mq_append(bi->free_queue, node, 1);
						continue;
					}

					mon = FAST_HASH(node->str) % MAX_THREAD;
//					printf("...................... %d \n", mon);
					mq_append(bi->node[mon].info_queue, node, 1);
//					NODE_LOCK(&bi->node[mon]);
//					list_add(&node->head, &bi->node[mon].info_str_head);
//					NODE_UNLOCK(&bi->node[mon]);
//					sem_post(&bi->node[mon].sem);

					p = x + 1;
				}
				else {
					idx = BUFMAX - (p - buffer);
					memmove(buffer, p, idx);
					break;
				}
			}
		}
		close(fd);
	}

	return -1;
}

static void btree_thread_ui_out(struct bthread_info *ui, const char *filename)
{
	int i;
	FILE *out = stdout;

	if (filename) {
		fprintf(stderr, "output %s\n", filename);
		out = fopen(filename, "w+");
		if (out == NULL)
			out = stdout;
	}

	for (i = 0; i < MAX_THREAD; i++)
		btree_print(ui->node[i].tree, (void (*)(void*, void*))userinfo_print, out);

	if (out != stdout)
		fclose(out);
}

static void btree_thread_ui_end(struct bthread_info *ui)
{
	int i;
	void *value_ptr;

	for (i = 0; i < MAX_THREAD; i++) {
		ui->node[i].eof = 1;
		sem_post(&ui->node[i].sem);
	}

	for (i = 0; i < MAX_THREAD; i++) {
		pthread_join(ui->node[i].thread, &value_ptr);
		sem_destroy(&ui->node[i].sem);
	}
	mq_free(ui->free_queue);
}

static void btree_thread_ui_free(struct bthread_info *ui)
{
	int i;

	for (i = 0; i < MAX_THREAD; i++) {
		store_close(ui->node[i].store);
		btree_close(ui->node[i].tree);
	}

	free(ui);
}

ui bthread_ui = {
	.init    = (void *(*)(void))                           btree_thread_ui_create,
//	.addfile = (int (*)(void*, const char *, int*, int*))  btree_thread_ui_addfile_fopen,
	.addfile = (int (*)(void*, const char *, int*, int*))  btree_thread_ui_addfile_open,
	.out     = (void (*)(void*, const char *))             btree_thread_ui_out,
	.free    = (void (*)(void *))                          btree_thread_ui_free,
	.end     = (void (*)(void *))                          btree_thread_ui_end,
};

