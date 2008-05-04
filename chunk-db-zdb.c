/*
 * ZunkDB back-end.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

#include <event.h>

#include "zunkfs.h"
#include "chunk-db.h"
#include "utils.h"
#include "mutex.h"
#include "base64.h"
#include "list.h"

struct zdb_info {
	struct sockaddr_in start_node;
	struct timeval timeout;
	unsigned max_concurrency;
};

struct node;

struct request {
	struct evbuffer *evbuf;
	struct event_base *base;
	unsigned char *chunk;
	const unsigned char *digest;
	struct list_head node_list;
	struct sockaddr_in *addr_list;
	unsigned addr_count;
	unsigned addr_index;
	unsigned addr_concurrency;
	unsigned done;
};

struct node {
	struct event connect_event;
	struct bufferevent *bev;
	struct sockaddr_in addr;
	int sk;
	struct request *request;
	struct list_head node_entry;
	struct timeval stamp;
};

#define CACHE_MAX	100

static LIST_HEAD(node_cache);
static LIST_HEAD(dead_nodes);
static unsigned cache_count = 0;
static DECLARE_MUTEX(cache_mutex);

static struct node dead_node;

static inline int same_addr(const struct sockaddr_in *a,
		const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

static inline int node_is_addr(const struct node *node, 
		const struct sockaddr_in *addr)
{
	return same_addr(&node->addr, addr);
}

static void free_node(struct node *node)
{
	if (node->request)
		node->request->addr_concurrency --;
	list_del(&node->node_entry);
	bufferevent_free(node->bev);
	close(node->sk);
	free(node);
}

static struct node *find_node(const struct sockaddr_in *sa)
{
	struct node *node, *next;
	struct timeval now;

	lock(&cache_mutex);
	list_for_each_entry(node, &node_cache, node_entry) {
		if (node_is_addr(node, sa)) {
			cache_count --;
			list_del(&node->node_entry);
			goto found;
		}
	}

	gettimeofday(&now, NULL);

	list_for_each_entry_safe(node, next, &dead_nodes, node_entry) {
		if (timercmp(&now, &node->stamp, >)) {
			free_node(node);
			continue;
		}

		if (node_is_addr(node, sa)) {
			node = &dead_node;
			goto found;
		}
	}
	
	node = NULL;
found:
	unlock(&cache_mutex);

	return node;
}

static void __cache_node(struct node *node)
{
	node->request = NULL;

	bufferevent_disable(node->bev, EV_READ|EV_WRITE);

	list_del(&node->node_entry);

	/*
	 * Nodes that didn't finish connecting before request
	 * finished are considered dead for the next minute.
	 */

	if (event_pending(&node->connect_event, EV_WRITE, NULL)) {
		event_del(&node->connect_event);
		close(node->sk);
		list_add(&node->node_entry, &dead_nodes);
		gettimeofday(&node->stamp, NULL);
		node->stamp.tv_sec += 60;
		return;
	}

	list_add(&node->node_entry, &node_cache);

	if (++cache_count > CACHE_MAX) {
		free_node(list_entry(node_cache.prev, struct node, node_entry));
		cache_count --;
	}
}

static void cache_node(struct node *node)
{
	lock(&cache_mutex);
	__cache_node(node);
	unlock(&cache_mutex);
}

static void __store_node(struct request *request, struct sockaddr_in *addr)
{
	struct sockaddr_in *uaddr;
	int i;

	for (i = 0; i < request->addr_count; i ++) {
		uaddr = &request->addr_list[i];
		if (same_addr(addr, uaddr))
			return;
	}

	uaddr = realloc(request->addr_list,
			sizeof(struct sockaddr_in) * (i + 1));
	if (!uaddr)
		return;

	uaddr[i] = *addr;
	request->addr_list = uaddr;
	request->addr_count ++;
}

static void store_node(struct request *request, char *addr_str)
{
	struct sockaddr_in addr;
	char *port;

	port = strchr(addr_str, ':');
	if (!port)
		return;

	*port++ = 0;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(port));

	if (!inet_aton(addr_str, &addr.sin_addr))
		return;

	__store_node(request, &addr);
}

#define FIND_CHUNK		"find_chunk"
#define FIND_CHUNK_LEN		(sizeof(FIND_CHUNK) - 1)
#define STORE_CHUNK		"store_chunk"
#define STORE_CHUNK_LEN		(sizeof(STORE_CHUNK) - 1)
#define REQUEST_DONE		"request_done"
#define REQUEST_DONE_LEN	(sizeof(REQUEST_DONE) - 1)
#define STORE_NODE		"store_node"
#define STORE_NODE_LEN		(sizeof(STORE_NODE) - 1)

static int proc_msg(const char *buf, size_t len, struct node *node)
{
	struct request *req = node->request;
	char *msg = alloca(len + 1);

	assert(msg != NULL);

	memcpy(msg, buf, len);
	msg[len] = 0;

	if (!strncmp(msg, STORE_CHUNK, STORE_CHUNK_LEN)) {
		msg += STORE_CHUNK_LEN + 1;
		if (req->chunk) {
			base64_decode(msg, req->chunk, CHUNK_SIZE);
			req->chunk = NULL;
		}

	} else if (!strncmp(msg, REQUEST_DONE, REQUEST_DONE_LEN)) {
		msg += REQUEST_DONE_LEN + 1;
		if (!strcmp(msg, digest_string(req->digest))) {
			req->done ++;
			req->addr_concurrency --;
			cache_node(node);
			return 1;
		}

	} else if (!strncmp(msg, STORE_NODE, STORE_NODE_LEN)) {
		msg += STORE_NODE_LEN + 1;
		store_node(req, msg);
	}

	return 0;
}

static void readcb(struct bufferevent *bev, void *arg)
{
	struct node *node = arg;
	const char *buf;
	const char *end;
	int drain_all = 0;

	for (;;) {
		buf = (const char *)EVBUFFER_DATA(bev->input);
		end = (const char *)evbuffer_find(bev->input,
				(u_char *)"\r\n", 2);
		if (!end)
			return;

		if (!drain_all)
			drain_all = proc_msg(buf, end - buf, node);
		evbuffer_drain(bev->input, end - buf + 2);
	}
}

static void errorcb(struct bufferevent *bev, short what, void *arg)
{
	struct node *node = arg;
	TRACE("node=%p\n", node);
	free_node(node);
}

static void try_connect(int fd, short what, void *arg)
{
	struct node *node = arg;
	
	TRACE("node=%p\n", node);
again:
	if (!connect(fd, (struct sockaddr *)&node->addr,
				sizeof(struct sockaddr_in)) ||
			errno == EISCONN) {
		TRACE("connected!\n");
		bufferevent_enable(node->bev, EV_READ|EV_WRITE);
		return;
	}

	TRACE("%s\n", strerror(errno));

	if (errno == EINTR)
		goto again;

	if (errno == EALREADY || errno == EINPROGRESS)
		event_add(&node->connect_event, NULL);
	else {
		/*
		 * this is just to let cache_node() that
		 * this is a dead node.
		 */
		event_add(&node->connect_event, NULL);
		cache_node(node);
		TRACE("connect failed\n");
	}
}

static void write_request(struct node *node, struct request *request)
{
	TRACE("write_request node=%s:%u request=%p\n",
			inet_ntoa(node->addr.sin_addr),
			ntohs(node->addr.sin_port),
			request);

	node->request = request;
	list_add(&node->node_entry, &request->node_list);

	bufferevent_base_set(request->base, node->bev);
	bufferevent_write(node->bev, EVBUFFER_DATA(request->evbuf),
			EVBUFFER_LENGTH(request->evbuf));
}

static int send_request_to(struct request *request,
		const struct sockaddr_in *addr)
{
	struct node *node;
	int fl;

	node = find_node(addr);
	if (node == &dead_node) {
		TRACE("dead node\n");
		return -EINVAL;
	}
	if (node) {
		write_request(node, request);
		bufferevent_enable(node->bev, EV_READ|EV_WRITE);
		return 0;
	}

	node = malloc(sizeof(struct node));
	if (!node)
		return -ENOMEM;

	node->addr = *addr;

	node->sk = socket(AF_INET, SOCK_STREAM, 0);
	if (node->sk < 0) {
		ERROR("socket: %s\n", strerror(errno));
		free(node);
		return -EIO;
	}

	fl = fcntl(node->sk, F_GETFL);
	fcntl(node->sk, F_SETFL, fl | O_NONBLOCK);

	node->bev = bufferevent_new(node->sk, readcb, NULL, errorcb, node);
	if (!node->bev) {
		ERROR("bufferevent_new: %s\n", strerror(errno));
		close(node->sk);
		free(node);
		return -EIO;
	}

	write_request(node, request);
	bufferevent_disable(node->bev, EV_READ | EV_WRITE);

	event_set(&node->connect_event, node->sk, EV_WRITE, try_connect, node);
	event_base_set(request->base, &node->connect_event);

	try_connect(node->sk, EV_WRITE, node);

	return 0;
}

static void timeout_cb(int fd, short event, void *arg)
{
	TRACE("request=%p\n", arg);
}

static int send_request(struct evbuffer *evbuf, struct zdb_info *db_info,
		const unsigned char *digest, unsigned char *chunk)
{
	struct timeval timeout = db_info->timeout;
	struct request request;
	struct event to_event;
	int err;

	request.evbuf = evbuf;
	request.chunk = chunk;
	request.digest = digest;
	list_head_init(&request.node_list);
	request.addr_list = NULL;
	request.addr_count = 0;
	request.addr_index = 0;
	request.addr_concurrency = 0;
	request.done = 0;

	request.base = event_base_new();
	if (!request.base) {
		ERROR("event_base: %s\n", strerror(errno));
		return -EIO;
	}

	__store_node(&request, &db_info->start_node);

	timeout_set(&to_event, timeout_cb, &request);
	event_base_set(request.base, &to_event);
	timeout_add(&to_event, &timeout);

	err = -EIO;
	for (;;) {
		while (request.addr_index != request.addr_count) {
			if (request.addr_concurrency >=
					db_info->max_concurrency)
				break;
			send_request_to(&request,
					&request.addr_list[request.addr_index]);
			request.addr_index ++;
			request.addr_concurrency ++;
		}
		if (!timeout_pending(&to_event, &timeout)) {
			err = -ETIMEDOUT;
			break;
		}
		if (list_empty(&request.node_list))
			break;
		if (event_base_loop(request.base, EVLOOP_ONCE))
			break;
		if (!request.done)
			continue;
		if (!chunk)
			err = CHUNK_SIZE;
		else if (!request.chunk && verify_chunk(chunk, digest)) {
			err = CHUNK_SIZE;
			break;
		} else {
			request.chunk = chunk;
			request.done --;
		}
	}

	lock(&cache_mutex);
	while (!list_empty(&request.node_list)) {
		__cache_node(list_entry(request.node_list.next, struct node,
					node_entry));
	}
	unlock(&cache_mutex);

	timeout_del(&to_event);

	free(request.addr_list);

	evbuffer_free(request.evbuf);
	event_base_free(request.base);

	return err;
}

static int zdb_read_chunk(unsigned char *chunk, const unsigned char *digest,
		void *db_info)
{
	struct evbuffer *request;

	TRACE("digest=%s\n", digest_string(digest));

	request = evbuffer_new();
	if (!request)
		return -ENOMEM;

	if (evbuffer_add_printf(request, "%s %s\r\n", FIND_CHUNK,
				digest_string(digest)) < 0) {
		TRACE("evbuffer_add failed\n");
		evbuffer_free(request);
		return -EIO;
	}

	return send_request(request, db_info, digest, chunk);
}

static int zdb_write_chunk(const unsigned char *chunk,
		const unsigned char *digest, void *db_info)
{
	struct evbuffer *request;

	TRACE("digest=%s\n", digest_string(digest));

	request = evbuffer_new();
	if (!request)
		return -ENOMEM;

	if (evbuffer_add_printf(request, "%s ", STORE_CHUNK) < 0 ||
			base64_encode_evbuf(request, chunk, CHUNK_SIZE) < 0 ||
			evbuffer_add(request, "\r\n", 2) < 0) {
		TRACE("evbuffer_add failed\n");
		evbuffer_free(request);
		return -EIO;
	}

	return send_request(request, db_info, digest, NULL);
}

static int parse_spec(const char *spec, struct zdb_info *zdb_info)
{
	char *addr, *port;
	char *spec_copy;
	char *opt;
	int opt_count;

	spec_copy = alloca(strlen(spec + 1));
	if (!spec_copy)
		return -ENOMEM;

	strcpy(spec_copy, spec);

	addr = NULL;

	for (opt_count = 0; (opt = strsep(&spec_copy, ",")); opt_count ++) {
		if (!opt_count) {
			addr = opt;
			port = strchr(addr, ':');
			if (!port) {
				ERROR("No port\n");
				return -EINVAL;
			}
			*port++ = 0;

			zdb_info->start_node.sin_family = AF_INET;
			zdb_info->start_node.sin_port = htons(atoi(port));

			if (!inet_aton(addr, &zdb_info->start_node.sin_addr))
				return -EINVAL;

		} else if (!strncmp(opt, "timeout=", 8)) {
			zdb_info->timeout.tv_sec = atoi(opt + 8);
			if (!zdb_info->timeout.tv_sec)
				return -EINVAL;

		} else if (!strncmp(opt, "concurrency=", 12)) {
			zdb_info->max_concurrency = atoi(opt + 12);
			if (!zdb_info->max_concurrency)
				return -EINVAL;

		} else {
			ERROR("Unknown option: %s\n", opt);
			return -EINVAL;
		}
	}

	if (!addr) {
		ERROR("No address.\n");
		return -EINVAL;
	}

	return 0;
}

static struct chunk_db *zdb_chunkdb_ctor(int mode, const char *spec)
{
	struct chunk_db *cdb;
	struct zdb_info *zdb_info;
	int err;

	if (strncmp(spec, "zunkdb:", 7))
		return NULL;

	spec += 7;

	cdb = malloc(sizeof(struct chunk_db) + sizeof(struct zdb_info));
	if (!cdb)
		return ERR_PTR(ENOMEM);

	zdb_info = (void *)(cdb + 1);
	cdb->db_info = zdb_info;

	zdb_info->timeout.tv_sec = 60;
	zdb_info->timeout.tv_usec = 0;

	zdb_info->max_concurrency = -1;

	cdb->read_chunk = zdb_read_chunk;
	cdb->write_chunk = (mode == CHUNKDB_RW) ? zdb_write_chunk : NULL;

	err = parse_spec(spec, zdb_info);
	if (!err)
		return cdb;

	free(cdb);
	return ERR_PTR(err);
}

static void __attribute__((constructor)) init_chunkdb_zdb(void)
{
	register_chunkdb(zdb_chunkdb_ctor);
}

