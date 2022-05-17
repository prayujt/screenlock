# slock version
VERSION = 2.0

# Customize below to fit your system

# paths
PREFIX = /usr/local

# includes and libs
INCS = -I. -I/usr/include
LIBS = -L/usr/lib -lc -lcrypt -lX11

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -DHAVE_SHADOW_H -DCOLOR1=\"black\" -DCOLOR2=\"\#005577\"
CFLAGS = -std=c99 -pedantic -Wall -Os ${INCS} ${CPPFLAGS}
LDFLAGS = -s ${LIBS}

CC = gcc
