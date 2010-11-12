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

#define MAX_THREAD 4
#define MAX_STR 128
#define MAX_BUF 1280

struct bthread_info;

struct info_node {
	struct list_head head;
	char str[MAX_STR];
};

struct mqueue {
	struct list_head  msg_list;
	sem_t             get_sem;
	pthread_mutex_t   lock;
	int               size;

	struct info_node *node[MAX_BUF];

	int front;
	int rear;
};


struct mqueue *mq_create(void)
{
	struct mqueue* mq;
	int i;

	mq = (struct mqueue *)malloc(sizeof(struct mqueue));
	memset(mq, 0, sizeof(struct mqueue));

	sem_init(&mq->get_sem, 0, MAX_BUF - 1);
	for (i = 0; i < MAX_BUF; i++) {
		mq->node[i] = (struct info_node *)malloc(sizeof(struct info_node));
	}

	mq->front = mq->rear = 0;

	return mq;
}

void mq_free(struct mqueue *mq)
{
	int i;
	sem_destroy(&mq->get_sem);
	for (i = 0; i < MAX_BUF; i++) {
		free(mq->node[i]);
	}
	
	free(mq);
}

void mq_append(struct mqueue *mq, struct info_node *node)
{
	mq->rear = (mq->rear + 1) % MAX_BUF;
	mq->node[mq->rear] = node;
	sem_post(&mq->get_sem);
}

struct info_node *mq_get(struct mqueue *mq)
{
	struct info_node *node = NULL;
	sem_wait(&mq->get_sem);
	node = mq->node[mq->front];
	mq->front = (mq->front + 1) % MAX_BUF;

	return node;
}

struct bthread_node {
	int id;
	int count;
	struct list_head info_str_head;
	struct bthread_info *bi;

	struct btree *tree;
	struct store *store;
	pthread_mutex_t mutex;
	sem_t sem;
	pthread_t thread;
	int eof;
};

#define NODE_LOCK(node)   pthread_mutex_lock(&(node)->mutex)
#define NODE_UNLOCK(node) pthread_mutex_unlock(&(node)->mutex)

struct bthread_info {
	struct bthread_node node[MAX_THREAD];
	struct mqueue *free_queue;
};

#if 0
static struct mqueue *mqueue_create(int max_size)
{
	struct mqueue* queue = NULL;

	queue = (struct mqueue *)malloc(sizeof(struct mqueue));
	if (queue == NULL) {
		return NULL;
	}
	memset(queue, 0, sizeof(struct mqueue));

	queue->size = max_size;
	sem_init(&queue->get_sem, 0, 0);
	pthread_mutex_init(&queue->lock, NULL);

	INIT_LIST_HEAD(&queue->msg_list);

	return queue;
}

static void mqueue_close(struct mqueue *queue)
{
	/*make the msg scheduler keep running*/
	sem_post(&queue->get_sem);
	sem_destroy(&queue->get_sem);
	pthread_mutex_destroy(&queue->lock);
	free(queue);
}

#if 1
#define QUEUE_LOCK(queue)    pthread_mutex_lock(&(queue)->lock)
#define QUEUE_UNLOCK(queue)  pthread_mutex_unlock(&(queue)->lock)
#else
#define QUEUE_LOCK(queue)   
#define QUEUE_UNLOCK(queue)
#endif

static int mqueue_send(struct mqueue *queue, struct info_node *node)
{
	QUEUE_LOCK(queue);
	list_add_tail(&node->head, &queue->msg_list);
	QUEUE_UNLOCK(queue);
	sem_post(&queue->get_sem);

	return 0;
}

static struct info_node *mqueue_get(struct mqueue *queue)
{
	struct info_node* node = NULL;
	struct list_head* pos;
	struct list_head* n;

	sem_wait(&queue->get_sem);

	QUEUE_LOCK(queue);
	list_for_each_safe( pos, n, &queue->msg_list ) {
		node = list_entry(pos, struct info_node, head);
		list_del(pos);
		break;
	}
	QUEUE_UNLOCK(queue);

	return node;
}
#endif

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

static int userinfo_insert(struct btree *tree, char *info_str)
{
	struct user_info new_data;
	char key[20] = {0, }, *p;

	if (tree == NULL || info_str == NULL)
		return -1;

	p = strchr(info_str, ',');

	memcpy(key, info_str, p - info_str);

	memset(&new_data, 0, sizeof(struct user_info));
	userinfo_parser(&new_data, info_str);

	btree_insert(tree, &new_data, key);

	return 0;
}

void *insert_thread(struct bthread_node *thread_node)
{
	struct info_node *str_node;
	while (!thread_node->eof) {
		if (sem_wait(&thread_node->sem) != 0) {
			printf("sem_wait error\n");
			break;
		}
		NODE_LOCK(thread_node);
		str_node = node_list_get(&thread_node->info_str_head);
		NODE_UNLOCK(thread_node);

		if (str_node) {
			thread_node->count++;
			userinfo_insert(thread_node->tree, str_node->str);
			mq_append(thread_node->bi->free_queue, str_node);
		}
		else
			break;
	} 

	printf("insert %d pthread exit\n", thread_node->count);
	pthread_exit(0);

	return NULL;
}

static struct bthread_info *btree_ui_create(void)
{
	int i;
	struct bthread_info *bi = (struct bthread_info*)malloc(sizeof(struct bthread_info));

	bi->free_queue = mq_create();

	for (i = 0; i < MAX_THREAD; i++) {
		bi->node[i].id    = i;
		bi->node[i].count = 0;
		bi->node[i].eof   = 0;
		bi->node[i].bi    = bi;
		bi->node[i].store = store_open_memory(sizeof(struct user_info), 1000);
		bi->node[i].tree  = btree_new_memory(bi->node[i].store, \
						(int(*)(const void *, const void *))userinfo_compare, (int (*)(void*, void*))userinfo_update);
		INIT_LIST_HEAD(&bi->node[i].info_str_head);
		sem_init(&bi->node[i].sem, 0, 0);

		pthread_create(&bi->node[i].thread, NULL, (void *(*)(void*))insert_thread, bi->node + i);
	}

	return bi;
}

static int btree_ui_addfile(struct bthread_info *bi, const char *filename)
{
	if (filename && bi) {
		FILE *fp;
		if ((fp  = fopen(filename, "r")) != NULL) {
//			int i = 0;
			while (!feof(fp)) {
//				printf("i=%d\n", i++);
				struct info_node *node = mq_get(bi->free_queue);

				if (fgets(node->str, 512, fp)) {
					int mon;

					if (strlen(node->str) < 20)
						continue;
					// 422302197802270338
					mon = (node->str[10] -'0') * 10 + (node->str[11] -'0');
					mon = mon % MAX_THREAD;
					NODE_LOCK(&bi->node[mon]);
					list_add(&node->head, &bi->node[mon].info_str_head);
					NODE_UNLOCK(&bi->node[mon]);
					sem_post(&bi->node[mon].sem);
				}
			}
			fclose(fp);

			return 0;
		}
	}

	return -1;
}

static void btree_ui_out(struct bthread_info *ui, const char *filename)
{
	int i;
	FILE *out = stdout;

	if (filename) {
		printf("output %s\n", filename);
		out = fopen(filename, "w+");
		if (out == NULL)
			out = stdout;
	}

	for (i = 0; i < MAX_THREAD; i++)
		btree_print(ui->node[i].tree, (void (*)(void*, void*))userinfo_print, out);

	if (out != stdout)
		fclose(out);
}

static void btree_ui_free(struct bthread_info *ui)
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
		store_close(ui->node[i].store);
		btree_close(ui->node[i].tree);
	}
	mq_free(ui->free_queue);

	free(ui);
}

ui bthread_ui = {
	.init    = (void *(*)(void))btree_ui_create,
	.addfile = (int (*)(void*, const char *))btree_ui_addfile,
	.out     = (void (*)(void*, const char *))btree_ui_out,
	.free    = (void (*)(void *))btree_ui_free
};

