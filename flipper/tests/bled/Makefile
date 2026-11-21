# make -C tests/bled test (issue #388): bled's GATT serial service state
# machine (src/lib/bled) with a fake controller, and rpcd behind it, under
# ASan/UBSan.  No CPU2.
MROOT	:= ../../..
NPB	:= $(MROOT)/src/lib/nanopb
FPB	:= $(MROOT)/src/lib/flipperpb
RPC	:= $(MROOT)/src/lib/rpcd
BLE	:= $(MROOT)/src/lib/bled
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall $(SAN) -DPB_ENABLE_MALLOC
SRC	:= $(wildcard $(NPB)/pb_*.c) $(wildcard $(FPB)/gen/*.pb.c) \
	   $(RPC)/rpcd.c $(BLE)/bleserial.c
B	:= build

test: $(B)/bstest
	./$(B)/bstest

$(B)/bstest: bstest.c $(SRC) $(BLE)/bleserial.h $(RPC)/rpcd.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(NPB) -I$(FPB) -I$(FPB)/gen -I$(RPC) -I$(BLE) \
	  -o $@ bstest.c $(SRC)

clean:
	rm -rf $(B)

.PHONY: test clean
