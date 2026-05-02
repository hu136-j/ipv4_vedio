CC = gcc
CFLAGS = -Wall -Wextra -g -pthread 
TARGET = server

SRCS = server.c meduim.c token.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
