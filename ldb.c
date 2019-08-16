
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>

#define LDBG_PORT 7609

static void fatal(const char *msg)
{
	fprintf(stderr, msg);
	fprintf(stderr, ", err: %s\n", strerror(errno));
	exit(-1);
}

static void setnonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		fatal("fcntl F_GETFL failed");
	}

	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) {
		fatal("fcntl F_SETFL failed");
	}
}

static void transmit(int fdin, int fdout)
{
	static char buf[4096];
	while (1) {
		ssize_t nread = read(fdin, buf, sizeof(buf));
		if (nread > 0) {
			write(fdout, buf, (size_t)nread);
		} else if (nread == 0) {
			exit(0);
		} else {
			int err = errno;
			if (err == EWOULDBLOCK || err == EAGAIN) {
				return;
			}
			fatal("read() failed");
		}
	}
}

int main(int argc, char **argv)
{
	const char *ip = "127.0.0.1";
	struct sockaddr_in sa;	
	socklen_t socklen;
	struct pollfd fds[2];
	int sock;
	int pollret;
	
	
	if (argc > 1) {
		ip = argv[1];
	}
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		fatal("socket() failed");
	}
	sa.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &sa.sin_addr);
	sa.sin_port = htons(LDBG_PORT);
	if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		fatal("failed to connect debugging server");
	}
	
	setnonblock(0);
	setnonblock(1);
	setnonblock(sock);
	
	fds[0].fd = 0;
	fds[0].events = POLLIN;
	fds[1].fd = sock;
	fds[1].events = POLLIN;
	while (1) {
		int i;
		pollret = poll(fds, 2, -1);
		if (pollret <= 0) {
			continue;
		}
		for (i = 0; i < 2; i++) {
			if (fds[i].revents & POLLIN) {
				if (fds[i].fd == 0) {
					transmit(0, sock);
				} else {
					transmit(sock, 1);
				}
			}
		}
	}
	
	return 0;
}		
