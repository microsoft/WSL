ARCH=$(shell uname -m)
CC=gcc
CFLAGS=-ggdb -Werror -Wno-format-truncation -Wno-format-overflow -D_GNU_SOURCE=1
LDFLAGS=-pthread -lutil -lmount
LDLIBFLAGS=-L.

TEST_BINARY=wsl_unit_tests

UNIT_TEST_HEADER=unittests.h

UNIT_TEST_OBJECTS=\
	dev_pt_common.o \
	auxv.o \
	binfmt.o \
	brk.o \
	cgroup.o \
	common.o \
	dev_pt.o \
	dev_pt_2.o \
	drvfs.o \
	dup.o \
	epoll.o \
	eventfd.o \
	execve.o \
	flock.o \
	fork.o \
	fscommon.o \
	fstab.o \
	get_set_id.o \
	getaddrinfo.o \
	gettime.o \
	inotify.o \
	interop.o \
	ioprio.o \
	keymgmt.o \
	lxtevent.o \
	lxtfs.o \
	lxtlog.o \
	lxtmount.o \
	lxtutil.o \
	madvise.o \
	mprotect.o \
	mremap.o \
	namespace.o \
	netlink.o \
	overlayfs.o \
	pipe.o \
	poll.o \
	random.o \
	resourcelimits.o \
	sched.o \
	sem.o \
	shm.o \
	socket_nonblock.o \
	splice.o \
	sysfs.o \
	sysinfo.o \
	timer.o \
	timerfd.o \
	tty.o \
	ttys.o \
	unittests.o \
	user.o \
	utimensat.o \
	vfsaccess.o \
	vnet.o \
	waitpid.o \
	wslpath.o \
	xattr.o

#__NR_select has been removed from arm64 unitstd.h
TEST_BINARY_x86_64_OBJECTS=\
	select.o

TEST_BINARY_x86_OBJECTS=$(TEST_BINARY_x86_64_OBJECTS)

TEST_BINARY_aarch64_OBJECTS=

TEST_BINARY_OBJECTS=$(UNIT_TEST_OBJECTS) $(TEST_BINARY_$(ARCH)_OBJECTS)

.PHONY: clean all

all: $(TEST_BINARY)

clean:
	rm -f *.o
	rm -f *.a
	rm -f $(TEST_BINARY)

$(TEST_BINARY): $(TEST_BINARY_OBJECTS)
	$(CC) $^ $(LDFLAGS) $(LDLIBFLAGS) -o $@
