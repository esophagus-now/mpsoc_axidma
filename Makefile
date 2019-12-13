example:	example.c
	gcc -Iinclude/ -o example example.c src/axidma.c src/pinner_fns.c

clean:
	rm -rf example
