# Berry (spec Phase 7c): the host tool berryc (R7c.2: compiles scripts to
# bytecode for pkg) and the generated constant tables.  The device build is
# in flipper/minix.mk (berry); both use berry_conf.h and generate/.
#   make -C src/lib/berry        -> build/berryc, generate/*.h
O	?= build
GEN	:= generate
SRC	:= $(wildcard src/*.c) default/be_port.c default/be_modtab.c gzberry.c
HDR	:= $(wildcard src/*.h) berry_conf.h
CC_HOST	?= cc

all: $(O)/berryc

# coc scans the sources for the const object declarations and writes the
# string table and object headers (upstream tools/coc).
$(GEN)/be_const_strtab.h: $(SRC) $(HDR) tools/coc/coc
	@mkdir -p $(GEN)
	python3 -E -s tools/coc/coc -o $(GEN) src default -c berry_conf.h
gen: $(GEN)/be_const_strtab.h

$(O)/berryc: $(SRC) $(HDR) $(GEN)/be_const_strtab.h
	@mkdir -p $(O)
	$(CC_HOST) -O2 -DBE_HOST -I. -Isrc -I$(GEN) -o $@ $(SRC) -lm

clean:
	rm -rf $(O) $(GEN)
.PHONY: all gen clean
