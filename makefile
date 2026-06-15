CC     = gcc
CFLAGS = -Wall -Wextra -g

OBJS = session.o daemon.o client.o pane.o

mux: $(OBJS)
	$(CC) $(CFLAGS) -o mux $(OBJS) -lutil -lncurses

session.o: session.c mux.h
	$(CC) $(CFLAGS) -c session.c

daemon.o: daemon.c mux.h
	$(CC) $(CFLAGS) -c daemon.c

client.o: client.c mux.h
	$(CC) $(CFLAGS) -c client.c

pane.o: pane.c mux.h
	$(CC) $(CFLAGS) -c pane.c

clean:
	rm -f mux *.o /tmp/mux.sock /tmp/mux.log
