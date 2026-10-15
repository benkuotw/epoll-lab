#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 10000   // default, override with -p
#define BUF_SIZE 1024
#define THREADS 1000 // default, override with -t

enum result { INIT,
	      FAILED_CREATE,
	      FAILED,
	      ECHOED };

static struct sockaddr_in server_addr;

// each thread: socket, connect, write the message, read the echo, close
static void *client_thread(void *arg)
{

	enum result *ret = arg;
	int fd;
	char wbuf[BUF_SIZE];
	char rbuf[BUF_SIZE];
	int n;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		*ret = FAILED;
		goto out;
	}

	if (connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
		*ret = FAILED;
		goto out;
	}

	snprintf(wbuf, BUF_SIZE, "thrd[%d]", gettid());
	if (write(fd, wbuf, strlen(wbuf)) != (ssize_t)strlen(wbuf)) {
		*ret = FAILED;
		goto out;
	}

	n = read(fd, rbuf, BUF_SIZE);
	if (n == (ssize_t)strlen(wbuf) && memcmp(rbuf, wbuf, n) == 0)
		*ret = ECHOED;
	else
		*ret = FAILED;

out:
	close(fd);
	return NULL;
}

void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-p PORT] [-t THREADS]\n"
		"  -p PORT     server port (default %d)\n"
		"  -t THREADS  number of threads, one connection each (default %d)\n",
		prog, PORT, THREADS);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	pthread_t *thrds;
	enum result *rets;
	int failed_create = 0, failed = 0, echoed = 0;
	int port = PORT, nthreads = THREADS;
	int opt;
	char *end;
	long val;

	while ((opt = getopt(argc, argv, "p:t:")) != -1) {
		switch (opt) {
		case 'p':
			errno = 0;
			val = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' || val < 1 || val > 65535) {
				fprintf(stderr, "invalid port: %s\n", optarg);
				usage(argv[0]);
			}
			port = val;
			break;
		case 't':
			errno = 0;
			val = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' || val < 1 || val > 1000000) {
				fprintf(stderr, "invalid number of threads: %s\n", optarg);
				usage(argv[0]);
			}
			nthreads = val;
			break;
		default:
			usage(argv[0]);
		}
	}
	if (optind < argc) // no positional arguments
		usage(argv[0]);

	thrds = malloc(nthreads * sizeof(*thrds));
	rets = malloc(nthreads * sizeof(*rets));
	if (thrds == NULL || rets == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	server_addr.sin_port = htons(port);

	for (int i = 0; i < nthreads; i++) {
		rets[i] = INIT;
		if (pthread_create(&thrds[i], NULL, client_thread, &rets[i]) != 0) {
			perror("pthread_create");
			rets[i] = FAILED_CREATE;
			failed_create++;
		}
	}

	for (int i = 0; i < nthreads; i++) {
		if (rets[i] == FAILED_CREATE)
			continue;
		pthread_join(thrds[i], NULL);
		if (rets[i] == FAILED)
			failed++;
		else if (rets[i] == ECHOED)
			echoed++;
	}

	printf("threads %d\nfailed_create: %d\nfailed: %d\nechoed:%d\n",
	       nthreads, failed_create, failed, echoed);
	free(thrds);
	free(rets);
	return 0;
}
