# make gzsigtest (spec RS.4, ACS.1, T33a part 2): gzsig (src/lib/gzsig, the
# SHA-512 + Ed25519 verifier and the update-file check that gzupdate and atea
# share) on the PC, under AddressSanitizer and UBSan: FIPS 180-4 and RFC 8032
# vectors, 64 signatures of the Python `cryptography` package with bad
# variants, and every ACS.1 file of tools/updatetest.py with the same verdict
# as gzupdate-check.py, plus a flipped bit at each byte of the good file.
#
#	make test [STEP=n]	build and run; STEP=n flips a bit at every n-th
#			byte only (tests/host: 3).  Without python3's
#			`cryptography` only the built-in vectors run.
#
# Run by tests/host, which the `build` CI job runs.
MROOT	:= ../../..
L	:= $(MROOT)/src/lib/gzsig
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu89 -O2 -g -Wall -Wextra $(SAN)
B	:= build
STEP	?= 1
SRC	:= $(L)/sha512.c $(L)/ed25519.c $(L)/gzu.c

test: $(B)/gzsigtest
	if python3 -I -c 'import cryptography' 2>/dev/null; then \
	  python3 -I $(MROOT)/flipper/tools/updatetest.py -o $(B)/acs1 && \
	  $(B)/gzsigtest $(B)/acs1 $(STEP); \
	else $(B)/gzsigtest && \
	  echo "gzsigtest: no python3 cryptography, ACS.1 files skipped"; fi

$(B)/gzsigtest: gzsigtest.c $(SRC) $(L)/gzsig.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(L) -o $@ gzsigtest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
