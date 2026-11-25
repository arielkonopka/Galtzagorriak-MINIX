# make -C tests/pgpenc test: libpgpenc (src/lib/pgpenc) and the pgpenc
# command on the PC under AddressSanitizer and UBSan; gpg makes a key,
# decrypts what we write and must get the file back (needs gpg).
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/pgpenc
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall -Wextra $(SAN)
B	:= build

test: $(B)/pgpenc
	sh ./gpgtest.sh ./$(B)/pgpenc

$(B)/pgpenc: $(MROOT)/flipper/cmds/pgpenc.c $(L)/pgpenc.c $(L)/pgpenc.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DHOST -I$(L) -o $@ $(MROOT)/flipper/cmds/pgpenc.c $(L)/pgpenc.c

clean:
	rm -rf $(B)

.PHONY: test clean
