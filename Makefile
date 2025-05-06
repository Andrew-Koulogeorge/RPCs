all: mylib.so server #test

mylib.o: mylib.c
	gcc -Wall -fPIC -DPIC -c mylib.c -I../include -L../lib -l dirtree

mylib.so: mylib.o
	ld -shared -o mylib.so mylib.o -ldl

server: server.c
	gcc -Wall -o server server.c -I../include -L../lib -l dirtree

# test: test.c
# 	gcc -Wall -o test.o test.c -I../include -L../lib -l dirtree

clean:
	rm -f *.o *.so