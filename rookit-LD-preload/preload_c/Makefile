CC      ?= gcc
CFLAGS  ?= -Wall -O2 -fPIC
LDFLAGS ?= -shared -nostartfiles

TARGET  = libprocesshider.so
SRC     = preload.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -ldl

clean:
	rm -f $(TARGET)
