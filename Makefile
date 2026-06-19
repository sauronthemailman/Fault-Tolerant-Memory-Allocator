CC=gcc
CFLAGS=-Wall -Wextra -Werror -O2 -fPIC
LDFLAGS=

LIB=liballocator.so
RUN=runme

# Explicit file lists for autograder comment/format discovery.
SRCS=allocator.c runme.c
HDRS=allocator.h

OBJ_LIB=allocator.o
OBJ_RUN=runme.o

.PHONY: all clean test

all: $(LIB) $(RUN)

$(OBJ_LIB): allocator.c $(HDRS)
	$(CC) $(CFLAGS) -c allocator.c -o $(OBJ_LIB)

$(LIB): $(OBJ_LIB)
	$(CC) -shared -o $(LIB) $(OBJ_LIB)

$(OBJ_RUN): runme.c $(HDRS)
	$(CC) $(CFLAGS) -c runme.c -o $(OBJ_RUN)

$(RUN): $(OBJ_RUN) $(LIB)
	$(CC) -o $(RUN) $(OBJ_RUN) -L. -lallocator -Wl,-rpath,'$$ORIGIN' $(LDFLAGS)

test: all
	./runme --seed 1 --storm 0 --size 8192

clean:
	rm -f *.o $(LIB) $(RUN)
