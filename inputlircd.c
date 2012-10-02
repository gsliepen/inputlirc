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
#include <glob.h>
#include <fnmatch.h>

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
static char *device = "/var/run/lirc/lircd";

static bool capture_modifiers = false;
static bool meta = false;
static bool alt = false;
static bool shift = false;
static bool ctrl = false;

static long repeat_time = 0L;
static struct timeval previous_input;
static struct timeval evdev_timeout;
static struct input_event previous_event;
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


static int open_evdev(char *name) {
	int fd;
	fd = open(name, O_RDONLY);
	if(fd < 0) {
		syslog(LOG_ERR, "Could not open %s: %s\n", name, strerror(errno));
		return -1;
	}

	char bits = 0;

	if(ioctl(fd, EVIOCGBIT(0, sizeof bits), &bits) < 0) {
		close(fd);
		syslog(LOG_ERR, "Could not read supported event types from %s: %s\n", name, strerror(errno));
		return -1;
	}

	if(!(bits & 2)) {
		close(fd);
		syslog(LOG_ERR, "%s does not support EV_KEY events\n", name);
		return -1;
	}

	if(grab) {
		if(ioctl(fd, EVIOCGRAB, 1) < 0) {
			close(fd);
			syslog(LOG_ERR, "Failed to grab %s: %s\n", name, strerror(errno));
			return -1;
		}
	}
	return fd;
}

static void rescan_evdevs(fd_set *permset) {
	evdev_t *evdev;
	int fd;
	for(evdev = evdevs; evdev; evdev = evdev->next) {
		if(evdev->fd == -999) {
			syslog(LOG_INFO, "Reading device: %s", evdev->name);
			fd = open_evdev(evdev->name);
			if(fd >= 0) {
				evdev->fd = fd;
				FD_SET(evdev->fd, permset);
				syslog(LOG_INFO, "Success!");
			}
		}
	}
}


static void add_evdev(char *name) {
	int fd;
	evdev_t *newdev;

	fd = open_evdev(name);
	if(fd < 0)
		return;

	newdev = xalloc(sizeof *newdev);
	newdev->fd = fd;
	newdev->name = strdup(name);
	newdev->next = evdevs;
	evdevs = newdev;
}


static void add_named(char *pattern) {
	int i, result, fd;
	char name[32];
	glob_t g;

	result = glob("/dev/input/event*", GLOB_NOSORT, NULL, &g);

	if(result == GLOB_NOMATCH) {
		fprintf(stderr, "No event devices found!\n");
		return;
	} else if(result) {
		fprintf(stderr, "Could not read /dev/input/event*: %s\n", strerror(errno));
		return;
	}

	for(i = 0; i < g.gl_pathc; i++) {
		fd = open(g.gl_pathv[i], O_RDONLY);
		if(fd < 0) {
			fprintf(stderr, "Could not open %s: %s\n", g.gl_pathv[i], strerror(errno));
			continue;
		}
		
		result = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		close(fd);
		if(result < 0) {
			fprintf(stderr, "Could not read name of event device %s: %s\n", g.gl_pathv[i], strerror(errno));
			continue;
		}
		
		name[(sizeof name) -1] = 0;
		if(!fnmatch(pattern, name, FNM_CASEFOLD))
			add_evdev(g.gl_pathv[i]);
	}

	globfree(&g);
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

static void processevent(evdev_t *evdev, fd_set *permset) {
	struct input_event event;
	char message[1000];
	int len;
	client_t *client, *prev, *next;

	if(read(evdev->fd, &event, sizeof event) != sizeof event) {
		syslog(LOG_ERR, "Error processing event from %s: %s\n", evdev->name, strerror(errno));
		FD_CLR(evdev->fd, permset);
		close(evdev->fd);
		evdev->fd = -999;
		return;
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
	if(event.code == previous_event.code && time_elapsed(&previous_input, &current) < repeat_time)
		repeat++;
	else 
		repeat = 0;

	if(KEY_NAME[event.code])
		len = snprintf(message, sizeof message, "%x %x %s%s%s%s%s %s\n", event.code, repeat, ctrl ? "CTRL_" : "", shift ? "SHIFT_" : "", alt ? "ALT_" : "", meta ? "META_" : "", KEY_NAME[event.code], evdev->name);
	else
		len = snprintf(message, sizeof message, "%x %x KEY_CODE_%d %s\n", event.code, repeat, event.code, evdev->name);

	previous_input = current;
	previous_event = event;
	
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
	int retselect;

	FD_ZERO(&permset);
	
	for(evdev = evdevs; evdev; evdev = evdev->next) {
		if(evdev->fd < 0)
			continue;
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
		
		//wait for 30 secs, then rescan devices
		evdev_timeout.tv_sec = 30;
		
		retselect = select(maxfd, &fdset, NULL, NULL, &evdev_timeout);
		if(retselect > 0) {
			for(evdev = evdevs; evdev; evdev = evdev->next)
				if(FD_ISSET(evdev->fd, &fdset))
					processevent(evdev, &permset);

			if(FD_ISSET(sockfd, &fdset))
				processnewclient();
		}
		else {
			if(retselect < 0) {
				if(errno == EINTR)
					continue;
				syslog(LOG_ERR, "Error during select(): %s\n", strerror(errno));
			}
			rescan_evdevs(&permset);
		}
	}
}


int main(int argc, char *argv[]) {
	char *user = "nobody";
	char *translation_path = NULL;
	int opt, i;
	bool foreground = false, named = false;

	gettimeofday(&previous_input, NULL);

	while((opt = getopt(argc, argv, "cd:gm:n:fu:r:t:")) != -1) {
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
			case 'n':
				named = true;
				add_named(optarg);
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

	if(argc <= optind && !named) {
		fprintf(stderr, "Not enough arguments.\n");
		return EX_USAGE;
	}

	openlog("inputlircd", LOG_PERROR, LOG_DAEMON);

	for(i = optind; i < argc; i++)
		add_evdev(argv[i]);

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

	if(!foreground) {
		closelog();
		daemon(0, 0);
		openlog("inputlircd", 0, LOG_DAEMON);
	}

	syslog(LOG_INFO, "Started");

	signal(SIGPIPE, SIG_IGN);

	main_loop();

	return 0;
}
