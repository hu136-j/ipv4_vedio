CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../meduim -pthread
COMMON_DIR = ../common
COMMON_OBJS = $(COMMON_DIR)/config.o $(COMMON_DIR)/logger.o $(COMMON_DIR)/heartbeat.o
TARGET = clinet
SRC = client.c

all: $(TARGET)

$(COMMON_DIR)/%.o: $(COMMON_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -o $(TARGET) $(SRC) $(COMMON_OBJS)

.PHONY: all clean

clean:
	rm -f $(TARGET) $(COMMON_DIR)/*.o
