# make -C tests/flipperpb test (issue #388): libflipperpb (the stock RPC
# protobuf, nanopb) on the PC under ASan/UBSan against golden frames made
# by Google's protobuf (mkgolden.py; golden.h is committed).
MROOT	:= ../../..
NPB	:= $(MROOT)/src/lib/nanopb
FPB	:= $(MROOT)/src/lib/flipperpb
CC	?= cc
SAN	:= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS	:= -std=gnu11 -O1 -g -Wall $(SAN) -DPB_ENABLE_MALLOC
SRC	:= $(wildcard $(NPB)/pb_*.c) $(wildcard $(FPB)/gen/*.pb.c)
B	:= build

test: $(B)/fpbtest
	./$(B)/fpbtest

$(B)/fpbtest: fpbtest.c golden.h $(SRC) $(wildcard $(FPB)/gen/*.pb.h)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -I$(NPB) -I$(FPB) -I$(FPB)/gen -o $@ fpbtest.c $(SRC)

golden:
	python3 mkgolden.py

clean:
	rm -rf $(B)

.PHONY: test golden clean
