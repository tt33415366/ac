/**
  * Another candidate for Apache benmark
  */
 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <errno.h>
 #include <fcntl.h>
 #include <string.h>
 #include <sys/time.h>
 #include <sys/epoll.h>
 #include <sys/types.h> 
 #include <sys/socket.h>
 #include <assert.h>

 #include <pthread.h>

 #include <netdb.h>
#if 0
#define DEBUG(fmt, ...) \
	fprintf(stdout, "[ DEBUG ] [ %s:%u ] " fmt, __func__, __LINE__,  ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...)
#endif
#define INFO(fmt, ...) \
	fprintf(stdout, "[ INFO ] " fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) \
	fprintf(stdout, "[ WARN ] " fmt,  ##__VA_ARGS__)
#define ERROR(fmt, ...) \
	fprintf(stderr, "[ ERROR ] " fmt, ##__VA_ARGS__)
	
enum conn_state {
	STAT_UNCONNECTED = 0,
	STAT_CONNECTING,
	STAT_CONNECTED,
	STAT_READ,
	__STAT_MAX,
 };
 
 struct connection {
 	int fd;
	int state;
	int events;			/* Current epoll events */
	int rwrite, rwrote;
	int gotheader;			/* Seen the header? */
	int read;				/* Bytes read */
	char cbuff[4096];
	unsigned long begin_time;
	unsigned long end_time;
 };
 
 struct th_param {
 	pthread_t tid;
	int epoll_fd;
	struct epoll_event *epoll_events;
	int started, done;			/* The number of connections that we have done */
	unsigned long long duration; 			/* Sum of (end_time - begin_time) */
 	int max_req_count;
	int concurrent_count;
 	char buff[8192];
#if __STDC_VERSION__ >= 199901L
	struct connection connections[];
#else
	struct connection connections[0];
#endif
 };

/* WARNING: 
  * 	The variable here should be considered as read only data in thread context.
  */
static struct reqest_config {
	struct sockaddr addr;
	socklen_t addrlen;
	const char *host; /* host */
	int port;
	const char *path, *cookie, *auth, *hdrs;
	int req_content_len;
	char req_content[8192];
}  req = {
	.cookie = "",
	.auth = "",
	.hdrs = "",
};

static void stop_connection(struct th_param *param, struct connection *conn);

static unsigned long usec() 
{
 	struct timeval tv = {0};
	
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000000 + tv.tv_usec);
}

static void usage(const char *program) 
{
	printf("usage: %s [options] url\n", program);
	printf("Options:\n"
		"    -k concurrent count\n"
		"    -t thread count\n"
		"    -c total request count\n");
}

static struct th_param *allocate_th_param(int cc, int max_req_count)
{
	 struct th_param *param = ( struct th_param *)calloc(1, sizeof(*param) + 
	 	cc * sizeof(struct connection));

	 if (!param) {
	 	/* OOM */
		WARN("Out of memory...\n");
		return NULL;
	 }
	 param->epoll_events = (struct epoll_event *)calloc(cc, sizeof(struct epoll_event));
	 if (!param->epoll_events) {
	 	free(param);
		WARN("Out of memory...\n");
		return NULL;
	 }
	 param->max_req_count = max_req_count;
	 param->concurrent_count = cc;

	 return param;
}

static void free_th_param(struct th_param *param)
{
	if (param)
		free((void *)param->epoll_events);
	free((void *)param);
}

static void setfdnonblocking(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void set_conn_state(struct th_param *param, struct connection *conn, enum conn_state state)
{
	int oev = conn->events;
	struct epoll_event ev = {0};
	/* Accepted from ab */
	int events_by_state[] = {
		0,
		EPOLLOUT,
		EPOLLIN,
		EPOLLIN
	};

	conn->state = state;
	switch (state) {
		case STAT_CONNECTING:
		/* case STAT_CONNECTED: */
		case STAT_READ:
			ev.data.ptr = conn;
			conn->events = ev.events = events_by_state[state];
			DEBUG("%s -> %d\n", oev ? "Modify" : "Add", conn->fd);
			if (epoll_ctl(param->epoll_fd, oev ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, conn->fd, &ev) < 0) {
				ERROR("epoll_ctl() failed with %s\n", strerror(errno));
				exit(-errno);
			}
			break;
		case STAT_UNCONNECTED:
			ev.data.ptr = conn;
			/* conn->events = ev.events = events_by_state[state]; */
			ev.events = oev;
			DEBUG("%s -> %d\n", "Del", conn->fd);
			if (epoll_ctl(param->epoll_fd, EPOLL_CTL_DEL, conn->fd, &ev) < 0) {
				ERROR("epoll_ctl() failed with %s\n", strerror(errno));
				exit(-errno);
			}
			break;
	}
}

static void start_connection(struct th_param *param, struct connection *conn)
{
	if (param->started >= param->max_req_count) {
		return ;
	}

	assert(conn->fd == 0);
	conn->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (conn->fd < 0) {
		/* Out of socket ? */
		ERROR("socket() failed with %s\n", strerror(errno));
		exit(-errno);
	}
	setfdnonblocking(conn->fd);
	conn->begin_time = usec();
	if (connect(conn->fd, &req.addr, req.addrlen) < 0 && (errno != EINPROGRESS)) {
		WARN("connect() failed with %s\n", strerror(errno));
		stop_connection(param, conn);
		start_connection(param, conn);
		return;
	}
	set_conn_state(param, conn, STAT_CONNECTING);
	param->started++;
}

static void stop_connection(struct th_param *param, struct connection *conn)
{
	if (conn->fd > 0) {
		set_conn_state(param, conn, STAT_UNCONNECTED);
		/* Expected a faster shutdown */
		/* shutdown(conn->fd, SHUT_RDWR); */
		close(conn->fd);
	}
	conn->end_time = usec();
	if (conn->read) {
		param->done++;
	} else {
		/* Try again */
		param->started--;
	}
	/* Make it ready for reuse */
	memset(conn, 0x0, sizeof(*conn));
	start_connection(param, conn);
}

static void write_requeset(struct th_param *param, struct connection *conn)
{
	if (!conn->rwrite) {
		conn->rwrite = req.req_content_len;
		conn->rwrote = 0;
	}
	do {
		int n = write(conn->fd, req.req_content + conn->rwrote, (conn->rwrite - conn->rwrote));
		if (n < 0) {
			if (errno == EAGAIN) {
				/* Silently try again at a later time */
				return;
			} else {
				stop_connection(param, conn);
			}
		}
		conn->rwrote += n;
	} while (conn->rwrite > conn->rwrote);
	/* Time to read */
	set_conn_state(param, conn, STAT_READ);
}

static int parse_header(struct connection *conn, const char *buff, int buff_len)
{
	if (!conn->gotheader) {
		char respcode[4] = {0};
		const char *hbuf;
		const char *s = strstr(buff, "\r\n\r\n");
		
		if (!s) {
			/* Not a full header, we have to keep it for the next round */
			if ((conn->read + buff_len) >= sizeof(conn->cbuff)) {
				/* The header is too large to be handled */
				return -1;
			}
			memcpy(&conn->cbuff[conn->read], buff, buff_len);
			return 0;
		} 
		conn->gotheader = 1;
		if (conn->read) {
			memcpy(&conn->cbuff[conn->read], buff, buff_len);
			hbuf =  conn->cbuff;
		} else {
			hbuf = buff;
		}
		/* Check response code */
		s = strstr(hbuf, "HTTP/");
		if (s && strlen(s) > strlen("HTTP/1.x_")) {
			snprintf(respcode, sizeof(respcode), "%s", s + strlen("HTTP/1.x_"));
		} else {
			/* No respcode */
			snprintf(respcode, sizeof(respcode), "500");
		}
		if (respcode[0] != '2') {
			/* Not expected */
			WARN("Respcode = %s\n", respcode);
			
		}
	}
	
	return 0;
		
}

static void read_response(struct th_param *param, struct connection *conn)
{
	int n;

	memset(param->buff, 0x0, sizeof(param->buff));
	n = read(conn->fd, param->buff, sizeof(param->buff));
	if (n < 0) {
		if ((errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)) {
			return;
		}
		stop_connection(param, conn);
		return;
	} else if (n == 0) {
		stop_connection(param, conn);
		return;
	}
	
	if (parse_header(conn, param->buff, n) < 0) {
		stop_connection(param, conn);
		return;
	}
	conn->read += n;
}

static void *thread_loop(void *p)
{
	struct th_param *param = (struct th_param *)p;
	int i;
	unsigned long begin;
	
	/* In case we also wanna try fork in the futrure */
	param->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if ((param->epoll_fd < 0) && (errno == EINVAL || errno == ENOSYS)) {
		param->epoll_fd = epoll_create(1024);
	}
	if (param->epoll_fd < 0) {
		return NULL;
	}

	begin = usec();
	for (i = 0; i < param->concurrent_count; i++) {
		start_connection(param, &param->connections[i]);
	}

	do {
		int nevent = epoll_wait(param->epoll_fd, param->epoll_events, 
						param->concurrent_count, -1);

		for (i = 0; i < nevent; i++) {
			struct epoll_event *ev = &param->epoll_events[i];
			struct connection *conn = (struct connection *)ev->data.ptr;

			if ((ev->events & EPOLLIN) || (ev->events & EPOLLPRI)) {
				/* Ready for read */
				DEBUG("Ready for reading\n");
				read_response(param, conn);
			} else if (ev->events & EPOLLOUT) {
				/* Ready for write */
				DEBUG("Ready for writing\n");
				set_conn_state(param, conn, STAT_CONNECTED);
				write_requeset(param, conn);
			} else {
				/* EPOLLHUP || EPOLLERR */
				/* stop and restart again on condition*/
				stop_connection(param , conn);
			}
		}
	} while (param->done < param->max_req_count);
	close(param->epoll_fd);

	param->duration = usec() - begin;
	printf("%f - %ld\n", (float)param->max_req_count * 1000000/ param->duration, param->duration);

	return NULL;
}

static void build_reqest(void)
{
	int n = snprintf(req.req_content, sizeof(req.req_content),
		    "%s %s HTTP/1.0\r\n"
		    "%s" "%s" "%s"
		    "%s" "\r\n",
		    "GET", req.path,
		    "", req.cookie, req.auth,
		    req.hdrs);
	if (n > sizeof(req.req_content)) {
		exit(-EXIT_FAILURE);
	}
	req.req_content_len = n;
}

static char *strlendup(const char *src, int len)
{
	void *p = malloc(len + 1); /* NULL-terminated */
	
	if (p) {
		memset(p, 0x0, len + 1);
		memcpy(p, src, len);
	}

	return p;
}

static int parse_url(const char *url)
{
	int port = 80;
	const  char *s, *e;

	/* We support http only for now */
	if (strlen(url) > 7 && strncmp(url, "http://", 7) == 0) {
		url += 7;
	}
	if ((s = strchr(url, '/')) == NULL) {
		ERROR("path is required.\n");
		return -1;
	}
	if ((e = strchr(url, ':')) != NULL) {
		if (e < s) {
			char _port[6] = {0};
			
			req.host = strlendup(url, e - url);
			strncpy(_port, e + 1, s - e);
			port = atoi(_port);
		}
	}

	if (!req.host) {
		req.host = strlendup(url, s - url);
	}
			
	req.path = strdup(s);
	req.port = port;
	
	return 0;
}

static int setup_addrinfo(void)
{
	int err, fd;
	char port[6];
	struct addrinfo hints = {0}, *res, *r;

	snprintf(port, sizeof(port), "%d", req.port);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	printf("%s , %s\n", req.host, port);
	err = getaddrinfo(req.host, port, &hints, &res);
	if (err) {
		/* gai_strerror */
		ERROR("getaddrinfo() failed with %s\n", gai_strerror(err));
		return -err;
	}
	for (r = res; r; r = r->ai_next) {
		fd = socket(r->ai_family, r->ai_socktype,
		       r->ai_protocol);
		if (fd < 0) 
			continue;
		if (connect(fd, r->ai_addr, r->ai_addrlen) < 0) {
			close(fd);
			continue;
		}
		close(fd);
		break;
	}
	
	if (r) {
		memcpy(&req.addr, r->ai_addr, sizeof(req.addr));
		req.addrlen = r->ai_addrlen;
	} else {
		/* No working address */
		err = -1;
		ERROR("No working address: %s\n", strerror(errno));
	}

	freeaddrinfo(res);
	
	return err;
}

static int __main(int k, int t, int c, const char *url)
{
	int i, ret = 0;
	float m;
	unsigned long long total_duration;
	int cc_per_thread = k / t;
	int max_req_per_thread = c / t;
	struct th_param **params /* = (struct th_param **)calloc(t, sizeof(*params)) */;

	if (parse_url(url) < 0) {
		ERROR("parse_url failed\n");
		return -1;
	}
	
	if ((ret = setup_addrinfo()) < 0) {
		goto cleanup;
	}
	build_reqest();
	
	params = (struct th_param **)calloc(t, sizeof(*params));
	if (!params) {
		ret = -ENOMEM;
		ERROR("Out of memory...\n");
		goto cleanup;
	}
	
	for (i = 0; i < t; i++) {
		if (i == (t -1)) {
			/* The last one */
			params[i] = allocate_th_param(k -(cc_per_thread * i), c - (max_req_per_thread * i));
		} else {
			params[i] = allocate_th_param(cc_per_thread, max_req_per_thread);
		}
		if (!params[i]) {
			ret = -ENOMEM;
			ERROR("Out of memory...\n");
			goto cleanup;
		}
			
		pthread_create(&params[i]->tid, NULL, &thread_loop, params[i]);
	}

	total_duration = 0;
	m = 0.0;
	for (i = 0; i < t; i++) {
		pthread_join(params[i]->tid, NULL);
		total_duration += params[i]->duration;
		m += (float)params[i]->max_req_count * 1000000/ params[i]->duration;
	}
	/* Output the report */
	printf("The %s%s completed with %f(%f) connections per second [ mean ]\n", req.host, req.path, 
		((float)(c * 1000000) / (total_duration / t)), m / t);
	
cleanup:
	if (req.host) {
		free((void *)req.host);
	}
	if (req.path) {
		free((void *)req.path);
	}
	if (params) {
		for (i = 0; i < t; i++) {
			if (params[i]) {
				if (ret == ENOMEM) {
					/* Ensure that there are no mallocs in the thread to use this
					  * killer function 
					  */
					pthread_kill(params[i]->tid);
				}
				free_th_param(params[i]);
				params[i] = NULL; 
			} else {
				/* We might not have any hole here*/
				break; 
			}
		}
		free((void *)params);
	}
		
	return ret;
}

 int main(int argc, char * const*argv)
 {
 	int opt;
	int k = 100, t = 4, c = 5000;
	const char *url = NULL;

	while ((opt = getopt(argc, argv, "k:c:t:")) != -1) {
		switch (opt) {
			case 'k':
				k = atoi(optarg);
				break;
			case 'c':
				c = atoi(optarg);
				break;
			case 't':
				t = atoi(optarg);
				break;
			default:
				usage(argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	if (c < k) {
		ERROR("Concurrent reqest count(%d) cannot be larger than total request count(%d)\n", 
				k, c);
		exit(EXIT_FAILURE);
	}
	if (k < t) {
		ERROR("Concurrent request count(%d) cannot be less than the running thread number(%d)\n", 
				k, t);
		exit(EXIT_FAILURE);
	}
	if (optind != (argc -1)) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	
	url = argv[optind];

	return __main(k, t, c, url);
}
