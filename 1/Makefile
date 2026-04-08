CC=gcc
CFLAGS=-Wall -Wextra -O2
TARGET=myprogram

all: $(TARGET)

$(TARGET): myprogram.c
	$(CC) $(CFLAGS) myprogram.c -o $(TARGET)

clean:
	rm -f $(TARGET) fileA fileB fileC fileD fileA.gz fileB.gz result.txt

.PHONY: all clean