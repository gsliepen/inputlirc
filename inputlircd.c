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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/ioctl.h>
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
#include <ctype.h>

#include </usr/include/linux/input.h>
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

static bool grab = false;
static int key_min = 88;
static char *device = "/dev/lircd";

static bool capture_modifiers = false;
static bool meta = false;
static bool alt = false;
static bool shift = false;
static bool ctrl = false;

static long repeat_time = 0L;
static struct timeval previous_input;
static int repeat = 0;

static void *xalloc(size_t size) {
	void *buf = malloc(size);
	if(!buf) {
		fprintf(stderr, "Could not allocate %zd bytes with malloc(): %s\n", size, strerror(errno));
		exit(EX_OSERR);
	}
	memset(buf, 0, size);
	return buf;
}

static void parse_translation_table(const char *path) {
	FILE *table;
	char *line = NULL;
	size_t line_size = 0;
	char event_name[100];
	char lirc_name[100];
	unsigned int i;

	if(!path)
		return;

	table = fopen(path, "r");
	if(!table) {
		fprintf(stderr, "Could not open translation table %s: %s\n", path, strerror(errno));
		return;
	}

	while(getline(&line, &line_size, table) >= 0) {
		if (sscanf(line, " %99s = %99s ", event_name, lirc_name) != 2)
			continue;

		event_name[99] = '\0';
		lirc_name[99] = '\0';
		if(strlen(event_name) < 1 || strlen(lirc_name) < 1)
			continue;

		if(!(i = strtoul(event_name, NULL, 0))) {
			for(i = 0; i < KEY_MAX; i++) {
				if (!KEY_NAME[i])
					continue;
				if(!strcmp(event_name, KEY_NAME[i]))
					break;
			}
		}

		if(i >= KEY_MAX)
			continue;

		KEY_NAME[i] = strdup(lirc_name);

		if(!KEY_NAME[i]) {
			fprintf(stderr, "strdup failure: %s\n", strerror(errno));
			exit(EX_OSERR);
		}
	}

	fclose(table);
	free(line);
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
		if(grab) {
			if(ioctl(newdev->fd, EVIOCGRAB, 1) < 0) {
				close(newdev->fd);
				free(newdev);
				fprintf(stderr, "Failed to grab %s: %s\n", argv[i], strerror(errno));
				continue;
			}
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

static long time_elapsed(struct timeval *last, struct timeval *current) {
	long seconds = current->tv_sec - last->tv_sec;
	return 1000000 * seconds + current->tv_usec - last->tv_usec;
}

static void processevent(evdev_t *evdev) {
	struct input_event event;
	char message[1000];
	int len;
	client_t *client, *prev, *next;

	if(read(evdev->fd, &event, sizeof event) != sizeof event) {
		syslog(LOG_ERR, "Error processing event from %s: %s\n", evdev->name, strerror(errno));
		exit(EX_OSERR);
	}
	
	if(event.type != EV_KEY)
		return;

	if(event.code > KEY_MAX || event.code < key_min)
		return;
	
	if(capture_modifiers) {
		if(event.code == KEY_LEFTCTRL || event.code == KEY_RIGHTCTRL) {
			ctrl = !!event.value;
			return;
		}
		if(event.code == KEY_LEFTSHIFT || event.code == KEY_RIGHTSHIFT) {
			shift = !!event.value;
			return;
		}
		if(event.code == KEY_LEFTALT || event.code == KEY_RIGHTALT) {
			alt = !!event.value;
			return;
		}
		if(event.code == KEY_LEFTMETA || event.code == KEY_RIGHTMETA) {
			meta = !!event.value;
			return;
		}
	}

	if(!event.value) 
		return;

	struct timeval current;
	gettimeofday(&current, NULL);
	if(time_elapsed(&previous_input, &current) < repeat_time)
		repeat++;
	else 
		repeat = 0;

	if(KEY_NAME[event.code])
		len = snprintf(message, sizeof message, "%x %x %s%s%s%s%s %s\n", event.code, repeat, ctrl ? "CTRL_" : "", shift ? "SHIFT_" : "", alt ? "ALT_" : "", meta ? "META_" : "", KEY_NAME[event.code], evdev->name);
	else
		len = snprintf(message, sizeof message, "%x %x KEY_CODE_%d %s\n", event.code, repeat, event.code, evdev->name);

	previous_input = current;
	
	for(client = clients; client; client = client->next) {
		if(write(client->fd, message, len) != len) {
			close(client->fd);
			client->fd = -1;
		}
	}

	for(prev = NULL, client = clients; client; client = next) {
		next = client->next;
		if(client->fd < 0) {
			if(prev)
				prev->next = client->next;
			else
				clients = client->next;
			free(client);
		} else {
			prev = client;
		}
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
	char *translation_path = NULL;
	int opt;
	bool foreground = false;

	gettimeofday(&previous_input, NULL);

	while((opt = getopt(argc, argv, "cd:gm:fu:r:t:")) != -1) {
                switch(opt) {
			case 'd':
				device = strdup(optarg);
				break;
			case 'g':
				grab = true;
				break;
			case 'c':
				capture_modifiers = true;
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
			case 'r':
				repeat_time = atoi(optarg) * 1000L;
				break;
			case 't':
				translation_path = strdup(optarg);
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

	parse_translation_table(translation_path);

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
