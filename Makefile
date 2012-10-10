all:lldbserver

CFLAGS=-g -Wall -Ileveldb/include/

LDFLAGS=-L/usr/local/lib/ -Lleveldb -levent -lleveldb -lpthread

lldbserver:lldbserver.o workqueue.o

lldbserver.o:lldbserver.c

workqueue.o:workqueue.c

.PHONY:clean

clean:
	rm *.o lldbserver 
