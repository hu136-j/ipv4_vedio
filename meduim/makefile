CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
COMMON_DIR = ../common
COMMON_OBJS = $(COMMON_DIR)/config.o $(COMMON_DIR)/logger.o
TARGET = server

SRCS = server.c meduim.c token.c

all: $(TARGET)

$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(SRCS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -o $(TARGET) $(SRCS) $(COMMON_OBJS)

clean:
	rm -f $(TARGET) $(COMMON_DIR)/*.o

.PHONY: all clean
