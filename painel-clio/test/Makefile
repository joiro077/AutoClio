# Testa a logica pura (sem ESP-IDF) no PC:  make -C test
CORE = ../main/core
SRC  = host_test.c $(wildcard $(CORE)/*.c)
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Wno-missing-field-initializers

run: host_test
	./host_test

host_test: $(SRC) $(wildcard $(CORE)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f host_test
