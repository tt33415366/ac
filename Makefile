EXEC = ac
CC = gcc
CFLAGS += -g
LDFLAGS += -lpthread
OBJS = $(patsubst %.c, %.o, $(wildcard *.c))

%.o: %.c
	$(CC) -c $(CFLAGS) $^ -o $@

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	@rm -rf ac *.o
