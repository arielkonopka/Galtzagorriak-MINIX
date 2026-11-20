# make -C tests/rpcd test (issue #388): the rpcd core (src/lib/rpcd) on the
# PC under ASan/UBSan with a fake platform and the golden frames of
# tests/flipperpb.
MROOT	:= ../../..
NPB	:= $(MROOT)/src/lib/nanopb
FPB	:= $(MROOT)/src/lib/flipperpb
RPC	:= $(MROOT)/src/lib/rpcd
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall $(SAN) -DPB_ENABLE_MALLOC
SRC	:= $(wildcard $(NPB)/pb_*.c) $(wildcard $(FPB)/gen/*.pb.c) $(RPC)/rpcd.c
B	:= build

test: $(B)/rpctest
	./$(B)/rpctest

$(B)/rpctest: rpctest.c $(SRC) $(RPC)/rpcd.h ../flipperpb/golden.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(NPB) -I$(FPB) -I$(FPB)/gen -I$(RPC) -o $@ rpctest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
