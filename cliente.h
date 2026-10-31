#ifndef CLIENTE_H
#define CLIENTE_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define SERVER_FIFO "fifo_controlador"
#define LINEBUF 512

void enviar_server(const char *msg);

#endif
