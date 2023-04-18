
.DEFAULT_GOAL: main
main: linenoise/linenoise.c main.c console.c utils.c
	$(CC) -Wall -I linenoise linenoise/linenoise.c main.c console.c utils.c -o main

.PHONY: clean
clean:
	rm ./main ./a.out