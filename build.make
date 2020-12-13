.POSIX:
.DEFAULT:
.SUFFIXES: .cpp .o
.PHONY: examples install

CXX = c++
CXXFLAGS = -g -O2 -fstack-protector-strong
DASYNQ_INC = -idirafter subprojects/libdasynq/include
HOSTCXX = $(CXX)
HOSTCXXFLAGS = $(CXXFLAGS)
LDFLAGS = -flto -Wl,--as-needed
PREFIX = /usr/local
VERSION = 0.9.0

all: src/dinit/main src/dinitcheck/main src/dinitctl/main src/shutdown/main

.cpp.o:
	$(CXX) -c -std=c++11 -fpie $(DASYNQ_INC) $(CXXFLAGS) -o $@ $< -I include
.o:
	$(CXX) -pie $(LDFLAGS) -o $@ $?

src/dinit/main: src/dinit/main.o \
	src/dinit/baseproc_service.o \
	src/dinit/log.o \
	src/dinit/run_child_proc.o \
	src/control.o \
	src/dinit.o \
	src/load_service.o \
	src/options_processing.o \
	src/proc_service.o \
	src/service.o

src/dinitcheck/main: src/dinitcheck/main.o \
	src/options_processing.o

src/dinitctl/main: src/dinitctl/main.o

src/shutdown/main: src/shutdown/main.o

install: all
	mkdir -p '$(DESTDIR)$(PREFIX)/sbin'
	cp -- src/dinit/main '$(DESTDIR)$(PREFIX)/sbin/dinit'
	cp -- src/dinitcheck/main '$(DESTDIR)$(PREFIX)/sbin/dinitcheck'
	cp -- src/dinitctl/main '$(DESTDIR)$(PREFIX)/sbin/dinitctl'
	cp -- src/shutdown/main '$(DESTDIR)$(PREFIX)/sbin/shutdown'
