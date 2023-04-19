
FLAG = -g -MMD -Wall
INC = linenoise/
SRC = ./src

FILE = $(wildcard $(SRC)/*.c)
FILE += linenoise/linenoise.c

OBJ_FILE = $(patsubst %.c, %.o, $(FILE))
DEP_FILE = $(patsubst %.c, %.d, $(FILE))

.DEFAULT_GOAL: main
main: $(OBJ_FILE)
	$(CC) $(FLAG) -I$(INC) -o $@ $^

-include $(DEP_FILE)
%.o : %.c
	$(CC) $(FLAG) -I$(INC) -c -o $@ $<

.PHONY: clean
clean:
	rm main ./src/*.o ./src/*.d linenoise/*.o linenoise/*.d