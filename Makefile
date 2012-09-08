all: udperf64

clean:
	rm udperf32 udperf64

udperf64: udperf.c
	gcc -Wall -ggdb -O0 -lrt -liw udperf.c -o udperf64

udperf32: udperf.c
	gcc -Wall -m32 -lrt -liw udperf.c -o udperf32