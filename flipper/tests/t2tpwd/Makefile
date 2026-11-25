# t2t_pwd (the NFC password copier's password rules): host test.
#	make test
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/libnfc
CC	?= cc
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	   -fno-sanitize-recover=undefined
B	:= build

test: $(B)/t2ttest
	cd $(B) && ./t2ttest

$(B)/t2ttest: t2ttest.c $(L)/t2t_pwd.c $(L)/t2t_pwd.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ t2ttest.c $(L)/t2t_pwd.c

clean:
	-rm -rf $(B)

.PHONY: test clean
