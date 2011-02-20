/*
 *   stunnel-ctrl  Universal SSL tunnel control program
 *   Copyright (c) 2011 Citrix Systems
 *                 All Rights Reserved
 *
 *   Author:       David Scott <dave.scott@eu.citrix.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *   In addition, as a special exception, Michal Trojnara gives
 *   permission to link the code of this program with the OpenSSL
 *   library (or with modified versions of OpenSSL that use the same
 *   license as OpenSSL), and distribute linked combinations including
 *   the two.  You must obey the GNU General Public License in all
 *   respects for all of the code used other than OpenSSL.  If you modify
 *   this file, you may extend this exception to your version of the
 *   file, but you are not obligated to do so.  If you do not wish to
 *   do so, delete this exception statement from your version.
 */

#include "common.h"

int ping(const char *control){
  struct sockaddr_un addr;
  uint16_t msg_size;
  char buffer[1024];
  int fd;
  ssize_t written, read;

  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, control, sizeof(addr.sun_path) - 1);

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) perror("socket");
  if (connect(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) perror("connect");

  printf("Sending ping to %s\n", control);
  msg_size = snprintf(buffer + sizeof(msg_size), sizeof(buffer) - sizeof(msg_size), "PING");
  *((uint16_t*)buffer) = msg_size;
  written = send(fd, buffer, msg_size + sizeof(msg_size), 0);
  if (written < 0) perror("send");

  read = recv(fd, buffer, sizeof(buffer), 0);
  if (read < 0) perror("read");
  printf("Received %d bytes [%s]\n", read, buffer + sizeof(uint16_t));

  return 0;
}

int main(int argc, char* argv[]) {
  if (argc < 3){
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "%s <control socket> <command> [args]\n", argv[0]);
	return 1;
  }
  if (strcasecmp(argv[2], "ping") == 0)
	return ping(argv[1]);

  fprintf(stderr, "Unknown command: %s\n", argv[2]);
	  
  return 1;
}
