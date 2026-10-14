#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 10000
#define BUF_SIZE 1024
#define MAX_EVENTS 1000

void handle_client(int client_fd)
{
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	char client_ip[INET_ADDRSTRLEN];
	int client_port;
	char buf[BUF_SIZE];
	ssize_t numRead;
	pid_t pid;

	client_addr_len = sizeof(client_addr);
	if (getpeername(client_fd, (struct sockaddr *)&client_addr, &client_addr_len) == -1) {
		perror("getpeername");
		client_ip[0] = '\0';
		client_port = -1;
	} else {
		inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
		client_port = ntohs(client_addr.sin_port);
	}

	pid = getpid();

	// read until EAGAIN or EWOULDBLOCK
	for (;;) {
		numRead = read(client_fd, buf, BUF_SIZE - 1);
		if (numRead == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return; // keep the connection open

			perror("read");
			break;
		}

		if (numRead == 0) // client closes the connection
			break;

		buf[numRead] = '\0';
		printf("[%d]Received from %s:%d: %s\n", pid, client_ip, client_port, buf);

		if (write(client_fd, buf, numRead) != numRead) {
			perror("write");
			break;
		}
	}

	if (close(client_fd) == -1) {
		perror("close");
	}
}

int main(void)
{
	int server_fd, client_fd;
	struct sockaddr_in server_addr;
	int epoll_fd, numReady;
	struct epoll_event ev, evs[MAX_EVENTS];

	// avoid client closes the connection while server writes data causing SIGPIPE
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("signal");
		exit(EXIT_FAILURE);
	}

	// nonblocking listener, man epoll.7
	server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	int optval = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
		perror("setsockopt(SO_REUSEADDR)");
		exit(EXIT_FAILURE);
	}
	// if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &optval,
	// 	       sizeof(optval)) == -1) {
	// 	perror("setsockopt(SO_REUSEPORT)");
	// 	exit(EXIT_FAILURE);
	// }

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(PORT);

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	if (listen(server_fd, SOMAXCONN) == -1) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	// LT listener, man epoll.7
	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
		perror("epoll_ctl");
		exit(EXIT_FAILURE);
	}

	// epoll_wait loop
	for (;;) {
		// blocks until a fd is ready
		numReady = epoll_wait(epoll_fd, evs, MAX_EVENTS, -1);
		if (numReady == -1) {
			if (errno == EINTR) // SIGSTOP/SIGCONT
				continue;
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		for (int i = 0; i < numReady; ++i) {
			if (evs[i].data.fd == server_fd) {
				client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
				if (client_fd == -1) {
					perror("accept4");
					continue;
				}

				// set EPOLLRDHUP for client closes connection or shuts down writing half
				// set EPOLLET on client connection
				ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
				ev.data.fd = client_fd;
				if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
					perror("epoll_ctl: client_fd");
					close(client_fd);
				}
			} else if (evs[i].events & (EPOLLHUP | EPOLLERR)) {
				if (close(evs[i].data.fd) == -1)
					perror("close");
			} else {
				// EPOLLIN or EPOLLRDHUP
				// process I/O until EAGAIN or EWOULDBLOCK
				handle_client(evs[i].data.fd);
			}
		}
	}

	close(server_fd);
	return 0;
}
