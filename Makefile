CC = gcc
CFLAGS = -g -Wall -fsanitize=address,undefined -std=c99
CFLAGS2 = -g -std=c99 -pthread

#ww: ww.c
#	$(CC) $(CFLAGS) -o $@ $^

threadww: threadww.c
	$(CC) $(CFLAGS2) -o $@ $^
