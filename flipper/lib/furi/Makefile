# libfuri (spec R5.1, R5.2): host tests.
#   make test       build test_furi and test_gui with the host compiler, run them
#   make gui-ref    rewrite the reference images tests/gui/*.pbm (look at them!)
#   make icons      regenerate src/gui/assets_icons.c from assets/icons (needs PIL)
# The MINIX build (libfuri.a, /bin/furitest, /bin/furihello) is in ../../minix.mk.
CC	?= cc
SAN	:= -fsanitize=undefined -fno-sanitize-recover=undefined
CFLAGS	?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(SAN)
INC	:= -Iinclude -Iinclude/gui -Iinclude/gui/modules -Iu8g2 -Isrc/gui
SRC	:= src/thread.c src/sync.c src/timer.c src/record.c src/string.c \
	   src/check.c src/fdwait.c src/port_host.c
GUI_SRC	:= $(wildcard src/gui/*.c src/gui/modules/*.c \
	   src/gui/modules/widget_elements/*.c) $(wildcard u8g2/*.c)
HDR	:= $(wildcard include/*.h include/*/*.h include/gui/*/*.h \
	   include/gui/modules/*/*.h src/*.h src/gui/*.h src/gui/*/*/*.h u8g2/*.h)
B	:= build

test: $(B)/test_furi $(B)/test_gui
	./$(B)/test_furi
	./$(B)/test_gui tests/gui

gui-ref: $(B)/test_gui
	./$(B)/test_gui -w tests/gui

$(B)/test_furi: test_furi.c $(SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DFURI_HOST $(INC) -o $@ test_furi.c $(SRC)

$(B)/test_gui: test_gui.c $(SRC) $(GUI_SRC) $(HDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DFURI_HOST $(INC) -o $@ test_gui.c $(SRC) $(GUI_SRC)

icons:
	python3 -I tools/icons.py assets/icons src/gui/assets_icons.c include/assets_icons.h

clean:
	-rm -rf $(B)

.PHONY: test gui-ref icons clean
