#
# Project: udptunnel
# File: Makefile
#
# Copyright (C) 2009 Daniel Meekins
# Contact: dmeekins - gmail
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# Uncomment appropriate one for the system this is compiling for
OS=LINUX
#OS=SOLARIS
#OS=CYGWIN

# Uncomment to build 32-bit binary (if on 64-bit system)
#M32=-m32

# Uncomment to build with debugging symbols
#DEBUG=-g

CC=gcc
CFLAGS=-Wall -Wshadow -Wpointer-arith ${M32} ${DEBUG} -D ${OS}

#-Wwrite-strings -Wall

ifeq (${OS}, SOLARIS)
LDFLAGS=-lnsl -lsocket -lresolv -lev -lpthread -lrt
endif

LDFLAGS=-lev 

all: udptunnel

#
# Main program
#
# discarted: udpserver.o udp_pipe.o
OBJS=socket.o message.o client.o list.o acl.o udpclient.o udp_pipe.o sendqueue.o
udptunnel: udptunnel.c ${OBJS}
	gcc ${CFLAGS} -o udptunnel udptunnel.c ${OBJS} ${LDFLAGS} -lev
	strip udptunnel

#
# Supporting code
#
sendqueue.o: sendqueue.c sendqueue.h list.h socket.h client.h message.h common.h
list.o: list.c list.h common.h
socket.o: socket.c socket.h common.h
client.o: client.c client.h common.h
message.o: message.c message.h common.h
acl.o: acl.c acl.h common.h
udpclient.o: udpclient.c list.h socket.h client.h message.h common.h
# udpserver.o: udpserver.c list.h socket.h client.h message.h acl.h common.h
udp_pipe.o: udp_pipe.c list.h socket.h client.h message.h acl.h common.h

#
# Clean compiled and temporary files
#
clean:
ifeq (${OS}, CYGWIN)
	rm -f udptunnel.exe
else
	rm -f udptunnel
endif
	rm -f *~ *.o helpers/*~
