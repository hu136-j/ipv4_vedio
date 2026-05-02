CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../meduim
LDFLAGS = -pthread
TARGET = clinet
SRC = client.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

.PHONY: clean

clean:
	rm -f $(TARGET)
