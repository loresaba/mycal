CFLAGS = -Wall -Wextra -O2 -I./include

all: mycal

mycal: mycal.c
	$(CC) $(CFLAGS) -o mycal mycal.c -lsqlite3

clean:
	rm -f mycal 
