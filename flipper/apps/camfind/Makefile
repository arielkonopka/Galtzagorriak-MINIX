# camfind core: host unit tests (spec task T13).
#   make test     build and run them with the host compiler
#   make oui OUI_CSV=path/to/oui.csv
#                 regenerate cfoui.c from the IEEE registry (not in the repo:
#                 https://standards-oui.ieee.org/oui/oui.csv) and vendors.txt
CC	?= cc
CFLAGS	?= -O2 -Wall -Wextra -std=c99 -fsanitize=address,undefined -g

test: test_cfcore
	./test_cfcore

test_cfcore: test_cfcore.c cfcore.c cfoui.c cfcore.h
	$(CC) $(CFLAGS) -o $@ test_cfcore.c cfcore.c cfoui.c -lm

oui:
	python3 -I ../../tools/mkoui.py $(OUI_CSV) vendors.txt cfoui.c

# The target build: the same sources with the ARM compiler, warnings on.
target-check:
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall -Wextra \
	   -ffreestanding -c cfcore.c cfoui.c -o /dev/null 2>&1 | grep -v "cannot specify" || true
	arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -std=gnu89 -Wall -Wextra \
	   -ffreestanding -c cfcore.c -o cfcore.o && arm-none-eabi-gcc -mcpu=cortex-m4 \
	   -mthumb -Os -c cfoui.c -o cfoui.o && arm-none-eabi-size cfcore.o cfoui.o; \
	   rm -f cfcore.o cfoui.o

clean:
	-rm -f test_cfcore

.PHONY: test oui target-check clean
