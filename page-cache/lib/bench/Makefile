# Папка с исходниками библиотеки (vtpc.c, vtpc_locked.c, util.c, *.h, sys.h)
LIBDIR ?= ..
CC     ?= gcc

LIBSRC     = $(LIBDIR)/vtpc.c $(LIBDIR)/vtpc_locked.c $(LIBDIR)/util.c
LIBHDR     = $(wildcard $(LIBDIR)/*.h)
# --no-undefined: неопределённые символы ловятся при сборке, а не при запуске
LIBCFLAGS  = -O2 -fPIC -shared -pthread -I$(LIBDIR) -Wl,--no-undefined
# FORTIFY выключен: иначе read() компилируется в __read_chk и LD_PRELOAD его не ловит
BENCHFLAGS = -O2 -Wall -Wextra -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

LIBS = libvtpc.so libvtpc_nora.so libvtpc_os.so libvtpc_os_nora.so

all: bench $(LIBS)

bench: bench.c
	$(CC) $(BENCHFLAGS) -o $@ $<

# мой кэш, O_DIRECT (в обход кэша ОС), readahead включён
libvtpc.so: $(LIBSRC) $(LIBHDR)
	$(CC) $(LIBCFLAGS) -o $@ $(LIBSRC)

# мой кэш, O_DIRECT, без readahead
libvtpc_nora.so: $(LIBSRC) $(LIBHDR)
	$(CC) $(LIBCFLAGS) -DVTPC_RA_ENABLED=0 -o $@ $(LIBSRC)

# мой кэш поверх кэша ОС (без O_DIRECT), readahead включён
libvtpc_os.so: $(LIBSRC) $(LIBHDR)
	$(CC) $(LIBCFLAGS) -DVTPC_USE_O_DIRECT=0 -o $@ $(LIBSRC)

# мой кэш поверх кэша ОС, без readahead
libvtpc_os_nora.so: $(LIBSRC) $(LIBHDR)
	$(CC) $(LIBCFLAGS) -DVTPC_USE_O_DIRECT=0 -DVTPC_RA_ENABLED=0 -o $@ $(LIBSRC)

clean:
	rm -f bench $(LIBS)

.PHONY: all clean