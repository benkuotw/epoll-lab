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

#define PORT 10000 // default, override with -p
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

// accept until EAGAIN or EWOULDBLOCK since nonblocking listener, instead of one epoll_wait() per connection
void accept_clients(int epoll_fd, int server_fd)
{
	struct epoll_event ev;
	int client_fd;

	for (;;) {
		client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client_fd == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break; // backlog drained
			} else {
				perror("accept4");
				continue;
			}
		}

		// set EPOLLRDHUP for client closes connection or shuts down writing half
		// set EPOLLET on client connection
		ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
		ev.data.fd = client_fd;
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
			perror("epoll_ctl: client_fd");
			close(client_fd);
		}
	}
}

void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-p PORT] [-e] [-a]\n"
		"  -p PORT  listen port (default %d)\n"
		"  -e       edge-triggered listener (default: level-triggered)\n"
		"  -a       accept until EAGAIN (default: one accept per wakeup)\n",
		prog, PORT);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	int pid;
	int server_fd, client_fd;
	struct sockaddr_in server_addr;
	int epoll_fd, numReady;
	struct epoll_event ev, evs[MAX_EVENTS];
	int port = PORT;
	int et_listener = 0, accept_loop = 0;
	int opt;
	char *end;
	long val;

	while ((opt = getopt(argc, argv, "p:ea")) != -1) {
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
		case 'e':
			et_listener = 1;
			break;
		case 'a':
			accept_loop = 1;
			break;
		default:
			usage(argv[0]);
		}
	}
	if (optind < argc) // no positional arguments
		usage(argv[0]);

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
	server_addr.sin_port = htons(port);

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

	// LT listener by default, man epoll.7
	ev.events = EPOLLIN | (et_listener ? EPOLLET : 0);
	ev.data.fd = server_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
		perror("epoll_ctl");
		exit(EXIT_FAILURE);
	}

	pid = getpid();
	printf("%s[%d]:listening on port %d, %s listener, %s\n",
	       argv[0],
	       pid,
	       port,
	       (et_listener == 0) ? "LT" : "ET",
	       (accept_loop == 0) ? "one accept per wakeup" : "accept until EAGAIN");

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
				if (accept_loop == 0) {
					// one accept per wakeup, man epoll.7
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
				} else {
					accept_clients(epoll_fd, server_fd);
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
