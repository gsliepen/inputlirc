/*
    inputlircd -- zeroconf LIRC daemon that reads from /dev/input/event devices
    Copyright (C) 2006  Guus Sliepen <guus@sliepen.eu.org>

    This program is free software; you can redistribute it and/or modify it
    under the terms of version 2 of the GNU General Public License as published
    by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sysexits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <pwd.h>

#include "input.h"
#include "names.h"

typedef struct evdev {
	char *name;
	int fd;
	struct evdev *next;
} evdev_t;

static evdev_t *evdevs = NULL;

typedef struct client {
	int fd;
	struct client *next;
} client_t;

static client_t *clients = NULL;

static int sockfd;

static int key_min = 88;
static char *device = "/dev/lircd";

static void *xalloc(size_t size) {
	void *buf = malloc(size);
	if(!buf) {
		fprintf(stderr, "Could not allocate %zd bytes with malloc(): %s\n", size, strerror(errno));
		exit(EX_OSERR);
	}
	memset(buf, 0, size);
	return buf;
}

static void add_evdevs(int argc, char *argv[]) {
	int i;
	evdev_t *newdev;

	for(i = 0; i < argc; i++) {
		newdev = xalloc(sizeof *newdev);
		newdev->fd = open(argv[i], O_RDONLY);
		if(newdev->fd < 0) {
			free(newdev);
			fprintf(stderr, "Could not open %s: %s\n", argv[i], strerror(errno));
			continue;
		}
		newdev->name = basename(strdup(argv[i]));
		newdev->next = evdevs;
		evdevs = newdev;
	}
}
	
static void add_unixsocket(void) {
	struct sockaddr_un sa = {0};
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	if(sockfd < 0) {
		fprintf(stderr, "Unable to create an AF_UNIX socket: %s\n", strerror(errno));
		exit(EX_OSERR);
	}

	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, device, sizeof sa.sun_path - 1);

	unlink(device);

	if(bind(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		fprintf(stderr, "Unable to bind AF_UNIX socket to %s: %s\n", device, strerror(errno));
		exit(EX_OSERR);
	}

	chmod(device, 0666);

	if(listen(sockfd, 3) < 0) {
		fprintf(stderr, "Unable to listen on AF_UNIX socket: %s\n", strerror(errno));
		exit(EX_OSERR);
	}
}


static void processnewclient(void) {
	client_t *newclient = xalloc(sizeof *newclient);

	newclient->fd = accept(sockfd, NULL, NULL);

	if(newclient->fd < 0) {
		free(newclient);
		if(errno == ECONNABORTED || errno == EINTR)
			return;
		syslog(LOG_ERR, "Error during accept(): %s\n", strerror(errno));
		exit(EX_OSERR);
	}

        int flags = fcntl(newclient->fd, F_GETFL);
        fcntl(newclient->fd, F_SETFL, flags | O_NONBLOCK);

	newclient->next = clients;
	clients = newclient;
}
	
static void processevent(evdev_t *evdev) {
	struct input_event event;
	char message[1000];
	int len;
	client_t *client, *prev;

	if(read(evdev->fd, &event, sizeof event) != sizeof event) {
		syslog(LOG_ERR, "Error processing event from %s: %s\n", evdev->name, strerror(errno));
		exit(EX_OSERR);
	}
	
	if(event.type != EV_KEY)
		return;

	if(!event.value || event.code > KEY_MAX || event.code < key_min)
		return;

	if(KEY_NAME[event.code])
		len = snprintf(message, sizeof message, "%x %x %s %s\n", event.code, 0, KEY_NAME[event.code], evdev->name);
	else
		len = snprintf(message, sizeof message, "%x %x KEY_CODE_%d %s\n", event.code, 0, event.code, evdev->name);

	for(client = clients; client; client = client->next) {
		if(write(client->fd, message, len) != len) {
			close(client->fd);
			client->fd = -1;
		}
	}

	for(prev = NULL, client = clients; client; client = client->next) {
		if(client->fd < 0) {
			if(prev)
				prev->next = client->next;
			else
				clients = client->next;
			free(client);
			if(prev)
				client = prev;
			else
				client = clients;
		}
		if(!client)
			break;
	}
}

static void main_loop(void) {
	fd_set permset;
	fd_set fdset;
	evdev_t *evdev;
	int maxfd = 0;

	FD_ZERO(&permset);
	
	for(evdev = evdevs; evdev; evdev = evdev->next) {
		FD_SET(evdev->fd, &permset);
		if(evdev->fd > maxfd)
			maxfd = evdev->fd;
	}
	
	FD_SET(sockfd, &permset);
	if(sockfd > maxfd)
		maxfd = sockfd;

	maxfd++;
	
	while(true) {
		fdset = permset;
		
		if(select(maxfd, &fdset, NULL, NULL, NULL) < 0) {
			if(errno == EINTR)
				continue;
			syslog(LOG_ERR, "Error during select(): %s\n", strerror(errno));
			exit(EX_OSERR);
		}

		for(evdev = evdevs; evdev; evdev = evdev->next)
			if(FD_ISSET(evdev->fd, &fdset))
				processevent(evdev);

		if(FD_ISSET(sockfd, &fdset))
			processnewclient();
	}
}

int main(int argc, char *argv[]) {
	char *user = "nobody";
	int opt;
	bool foreground = false;

        while((opt = getopt(argc, argv, "d:m:fu:")) != -1) {
                switch(opt) {
			case 'd':
				device = strdup(optarg);
				break;
			case 'm':
				key_min = atoi(optarg);
				break;
			case 'u':
				user = strdup(optarg);
				break;
			case 'f':
				foreground = true;
				break;
                        default:
				fprintf(stderr, "Unknown option!\n");
                                return EX_USAGE;
                }
        }

	if(argc <= optind) {
		fprintf(stderr, "Not enough arguments.\n");
		return EX_USAGE;
	}

	add_evdevs(argc - optind, argv + optind);

	if(!evdevs) {
		fprintf(stderr, "Unable to open any event device!\n");
		return EX_OSERR;
	}

	add_unixsocket();

	struct passwd *pwd = getpwnam(user);
	if(!pwd) {
		fprintf(stderr, "Unable to resolve user %s!\n", user);
		return EX_OSERR;
	}

	if(setgid(pwd->pw_gid) || setuid(pwd->pw_uid)) {
		fprintf(stderr, "Unable to setuid/setguid to %s!\n", user);
		return EX_OSERR;
	}

	if(!foreground)
		daemon(0, 0);

	syslog(LOG_INFO, "Started");

	signal(SIGPIPE, SIG_IGN);

	main_loop();

	return 0;
}
