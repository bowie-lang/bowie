CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -Isrc
LDFLAGS = -lm
TARGET  = bowie
SRCS    = src/lexer.c src/ast.c src/object.c src/env.c \
          src/parser.c src/interpreter.c src/builtins.c \
          src/http.c src/main.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/bowie

clean:
	rm -f $(OBJS) $(TARGET)
