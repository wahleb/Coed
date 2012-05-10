
coed: main.c
	gcc -Wall -lncurses -lm -lrt -lpthread main.c -o coed -O2

fuzz: fuzz.c
	gcc -Wall fuzz.c -o fuzz -O2

run_tests: fuzz coed
	$(foreach out,log1 log2 log3,screen -d -m bash -c "./fuzz | ./coed test 2> $(out)" &)

test: test.c
	gcc -Wall -lncurses test.c
