CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../meduim1
LDFLAGS = -pthread
TARGET = clinet
SRC = client.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

.PHONY: clean

clean:
	rm -f $(TARGET)
