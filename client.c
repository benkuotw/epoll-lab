#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 10000
#define BUF_SIZE 1024
#define THREADS 1000

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

int main(int argc, char *argv[])
{
	pthread_t thrds[THREADS];
	enum result rets[THREADS];
	int failed_create = 0, failed = 0, echoed = 0;

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	server_addr.sin_port = htons(PORT);

	for (int i = 0; i < THREADS; i++) {
		rets[i] = INIT;
		if (pthread_create(&thrds[i], NULL, client_thread, &rets[i]) != 0) {
			perror("pthread_create");
			rets[i] = FAILED_CREATE;
			failed_create++;
		}
	}

	for (int i = 0; i < THREADS; i++) {
		if (rets[i] == FAILED_CREATE)
			continue;
		pthread_join(thrds[i], NULL);
		if (rets[i] == FAILED)
			failed++;
		else if (rets[i] == ECHOED)
			echoed++;
	}

	printf("threads %d\nfailed_create: %d\nfailed: %d\nechoed:%d\n",
	       THREADS, failed_create, failed, echoed);
	return 0;
}
