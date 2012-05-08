
coed: main.c
	gcc -Wall -lncurses -lrt -lpthread main.c -o coed -O2

fuzz: fuzz.c
	gcc -Wall fuzz.c -o fuzz -O2

test: test.c
	gcc -Wall -lncurses test.c
