
coed: main.c
	gcc -Wall main.c -lncurses -lm -lrt -lpthread -o coed -O2
#just ignore the rest
fuzz: fuzz.c
	gcc -Wall fuzz.c -o fuzz -O2

run_tests: fuzz coed
	$(foreach out,log1 log2 log3,screen -d -m bash -c "./fuzz | ./coed test 2> $(out)" &)

test: test.c
	gcc -Wall -lncurses test.c
