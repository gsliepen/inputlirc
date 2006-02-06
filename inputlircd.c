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
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sysexits.h>
#include <sys/stat.h>
#include <fcntl.h>

#define EX_FAIL 1

#include "input.h"
#include "names.h"

int main(int argc, char *argv[]) {
	char *remote = basename(strdup(argv[1]));
	
	int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	if(sockfd < 0) {
		perror("socket");
		return EX_FAIL;
	}

	struct sockaddr_un sa = {
		.sun_family = AF_UNIX,
		.sun_path = "/dev/lircd",
	};

	unlink(sa.sun_path);

	if(bind(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind");
		return EX_FAIL;
	}

	chmod(sa.sun_path, 0666);

	if(listen(sockfd, 3) < 0) {
		perror("listen");
		return EX_FAIL;
	}

	int eventfd = open(argv[1], O_RDONLY);

	if(eventfd < 0) {
		perror("open");
		return EX_FAIL;
	}

	int cfd = accept(sockfd, NULL, NULL);

	if(cfd < 0) {
		perror("accept");
		return EX_FAIL;
	}

	while(true) {
		struct input_event event;

		if(read(eventfd, &event, sizeof event) <= 0) {
			perror("read");
			return EX_FAIL;
		}
		
		switch(event.type) {
			case EV_KEY:
				if(event.value) {
					char buf[100];
					int len = snprintf(buf, sizeof buf, "%x %x %s %s\n", event.code, 0, KEY_NAME[event.code], remote);
					if(len <= 0) {
						perror("snprintf");
						return EX_FAIL;
					}

					if(write(cfd, buf, len) <= 0) {
						perror("write");
						return EX_FAIL;
					}
				}
				break;
			default:
				break;
		}
	}
}
