
FLAG = -g -Og -MMD -Wall
INC_LINENOISE = linenoise/
INC_HIREDIS = hiredis/
SRC = ./src

FILE = $(wildcard $(SRC)/*.c)
FILE += linenoise/linenoise.c

OBJ_FILE = $(patsubst %.c, %.o, $(FILE))
DEP_FILE = $(patsubst %.c, %.d, $(FILE))

.DEFAULT_GOAL: main
main: $(OBJ_FILE)
	$(MAKE) -C $(INC_HIREDIS)
	$(CC) $(FLAG) -I$(INC_LINENOISE) -I$(INC_HIREDIS) -o $@ $^ hiredis/libhiredis.a

-include $(DEP_FILE)
%.o : %.c
	$(CC) $(FLAG) -I$(INC_LINENOISE) -I$(INC_HIREDIS) -c -o $@ $<

.PHONY: clean
clean:
	$(MAKE) clean -C $(INC_HIREDIS)
	rm main ./src/*.o ./src/*.d linenoise/*.o linenoise/*.d