
coed: main.c
	gcc -Wall -lncurses -lrt -lpthread main.c -o coed -O2

fuzz: fuzz.c
	gcc -Wall fuzz.c -o fuzz -O2

test: fuzz coed
	$(foreach out,log1 log2 log3,screen -d -m bash -c "./fuzz | ./coed test 2> $(out)" &)
