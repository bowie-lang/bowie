CC      = cc
PKG_CONFIG ?= pkg-config

# PostgreSQL client (libpq): pkg-config, common Homebrew keg paths, or stub.
ifneq ($(strip $(FORCE_POSTGRES_STUB)),1)
  ifeq ($(shell $(PKG_CONFIG) --exists libpq 2>/dev/null && echo ok),ok)
    POSTGRES_MODULE := src/postgres.c
    POSTGRES_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpq)
    POSTGRES_LIBS   := $(shell $(PKG_CONFIG) --libs libpq)
  else ifneq ($(wildcard /opt/homebrew/opt/libpq/include/libpq-fe.h),)
    POSTGRES_MODULE := src/postgres.c
    POSTGRES_CFLAGS := -I/opt/homebrew/opt/libpq/include
    POSTGRES_LIBS   := -L/opt/homebrew/opt/libpq/lib -lpq
  else ifneq ($(wildcard /usr/local/opt/libpq/include/libpq-fe.h),)
    POSTGRES_MODULE := src/postgres.c
    POSTGRES_CFLAGS := -I/usr/local/opt/libpq/include
    POSTGRES_LIBS   := -L/usr/local/opt/libpq/lib -lpq
  else
    POSTGRES_MODULE := src/postgres_disabled.c
    POSTGRES_CFLAGS :=
    POSTGRES_LIBS   :=
  endif
else
  POSTGRES_MODULE := src/postgres_disabled.c
  POSTGRES_CFLAGS :=
  POSTGRES_LIBS   :=
endif

# libcurl (fetch HTTPS support): pkg-config, common Homebrew keg paths, or disabled.
ifneq ($(strip $(FORCE_CURL_STUB)),1)
  ifeq ($(shell $(PKG_CONFIG) --exists libcurl 2>/dev/null && echo ok),ok)
    CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl) -DBOWIE_CURL
    CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)
  else ifneq ($(wildcard /opt/homebrew/opt/curl/include/curl/curl.h),)
    CURL_CFLAGS := -I/opt/homebrew/opt/curl/include -DBOWIE_CURL
    CURL_LIBS   := -L/opt/homebrew/opt/curl/lib -lcurl
  else ifneq ($(wildcard /usr/local/opt/curl/include/curl/curl.h),)
    CURL_CFLAGS := -I/usr/local/opt/curl/include -DBOWIE_CURL
    CURL_LIBS   := -L/usr/local/opt/curl/lib -lcurl
  else
    CURL_CFLAGS :=
    CURL_LIBS   :=
  endif
else
  CURL_CFLAGS :=
  CURL_LIBS   :=
endif

CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -Wall -Wextra -Wno-unused-parameter -O2 -Isrc $(POSTGRES_CFLAGS) $(CURL_CFLAGS)
LDFLAGS = -lm $(POSTGRES_LIBS) $(CURL_LIBS)
TARGET  = bowie
SRCS    = src/lexer.c src/ast.c src/object.c src/env.c \
          src/parser.c src/interpreter.c src/builtins.c \
          src/mustache.c src/http.c src/coro.c src/event_loop.c \
          $(POSTGRES_MODULE) src/main.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/bowie
	mkdir -p /usr/local/lib/bowie/std
	cp std/*.bow /usr/local/lib/bowie/std/

clean:
	rm -f src/lexer.o src/ast.o src/object.o src/env.o src/parser.o \
	      src/interpreter.o src/builtins.o src/mustache.o src/http.o src/coro.o \
	      src/event_loop.o src/postgres.o \
	      src/postgres_disabled.o src/main.o $(TARGET)
