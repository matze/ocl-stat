CFLAGS = `pkg-config --cflags glib-2.0`
LIBS = `pkg-config --libs glib.2.0`

.PHONY: all clean

all: libocl-stat.so

clean:
	rm -f libocl-stat.so ocl-stat.o

%.o: %.c
	$(CC) -fPIC -rdynamic -g -c $(CFLAGS) $<

libocl-stat.so: ocl-stat.o
	$(CC) -shared -Wl,-soname,$@ -o $@ $^ -lc -ldl $(LIBS)
