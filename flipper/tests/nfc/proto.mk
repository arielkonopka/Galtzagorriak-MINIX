# proto.mk - the vendored Unleashed pollers (src/lib/nfc) over the libnfc
# shim (src/lib/libnfc/nfc_shim.c, shim/) and nfc_xfer, against software
# cards (cardmodel.h); issue #404.  Included by Makefile; `make test` runs
# it after nfctest.  Vendored files are compiled unchanged.
V	:= $(MROOT)/src/lib/nfc
VINC	:= -I$(L)/shim -I$(L) -I$(V) -I$(V)/nfc -I.
# -Wno-format: stock prints uint32_t with %lu (unsigned long on ARM).
VSRC	:= $(L)/nfc_shim.c $(L)/nfc_poller_defs_shim.c \
	   $(V)/nfc/nfc_poller.c $(V)/nfc/protocols/nfc_protocol.c \
	   $(V)/toolbox/bit_buffer.c $(V)/toolbox/simple_array.c \
	   $(V)/nfc/helpers/iso14443_crc.c $(V)/nfc/helpers/iso14443_4_layer.c \
	   $(V)/nfc/protocols/iso14443_3a/iso14443_3a.c \
	   $(V)/nfc/protocols/iso14443_3a/iso14443_3a_poller.c \
	   $(V)/nfc/protocols/iso14443_3a/iso14443_3a_poller_i.c \
	   $(V)/nfc/protocols/iso14443_4a/iso14443_4a.c \
	   $(V)/nfc/protocols/iso14443_4a/iso14443_4a_i.c \
	   $(V)/nfc/protocols/iso14443_4a/iso14443_4a_poller.c \
	   $(V)/nfc/protocols/iso14443_4a/iso14443_4a_poller_i.c \
	   $(V)/nfc/protocols/iso14443_3b/iso14443_3b.c \
	   $(V)/nfc/protocols/iso14443_3b/iso14443_3b_poller.c \
	   $(V)/nfc/protocols/iso14443_3b/iso14443_3b_poller_i.c \
	   $(V)/nfc/protocols/iso14443_4b/iso14443_4b.c \
	   $(V)/nfc/protocols/iso14443_4b/iso14443_4b_i.c \
	   $(V)/nfc/protocols/iso14443_4b/iso14443_4b_poller.c \
	   $(V)/nfc/protocols/iso14443_4b/iso14443_4b_poller_i.c \
	   $(V)/nfc/helpers/nxp_native_command.c $(V)/bit_lib/bit_lib.c \
	   $(V)/nfc/protocols/mf_desfire/mf_desfire.c \
	   $(V)/nfc/protocols/mf_desfire/mf_desfire_i.c \
	   $(V)/nfc/protocols/mf_desfire/mf_desfire_poller.c \
	   $(V)/nfc/protocols/mf_desfire/mf_desfire_poller_i.c \
	   $(V)/nfc/helpers/iso13239_crc.c \
	   $(V)/nfc/protocols/iso15693_3/iso15693_3.c \
	   $(V)/nfc/protocols/iso15693_3/iso15693_3_i.c \
	   $(V)/nfc/protocols/iso15693_3/iso15693_3_poller.c \
	   $(V)/nfc/protocols/iso15693_3/iso15693_3_poller_i.c \
	   $(V)/nfc/protocols/slix/slix.c $(V)/nfc/protocols/slix/slix_i.c \
	   $(V)/nfc/protocols/slix/slix_poller.c $(V)/nfc/protocols/slix/slix_poller_i.c
VHDR	:= $(wildcard $(L)/shim/*.h $(L)/shim/*/*.h) cardmodel.h cm_desfire.h cm_15693.h
# MIFARE Classic: the vendored poller and its sync layer (part 4, second half).
VSRC	+= $(L)/nfc_shim_mfc.c \
	   $(V)/nfc/helpers/crypto1.c $(V)/nfc/helpers/nfc_util.c $(V)/toolbox/hex.c \
	   $(V)/nfc/protocols/mf_classic/mf_classic.c \
	   $(V)/nfc/protocols/mf_classic/mf_classic_poller.c \
	   $(V)/nfc/protocols/mf_classic/mf_classic_poller_i.c \
	   $(V)/nfc/protocols/mf_classic/mf_classic_poller_sync.c
VHDR	+= cm_mfc.h
# -Wno-unused-variable: mf_classic_poller.c keeps a few.
VWARN	:= -Wno-format -Wno-unused-variable -Wno-unused-function

prototest: $(B)/prototest
	./$(B)/prototest

$(B)/prototest: prototest.c $(VSRC) $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -o $@ prototest.c $(VSRC) -lm

cardtest: $(B)/cardtest
	./$(B)/cardtest

$(B)/cardtest: cardtest.c $(VSRC) $(CSRC) $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -o $@ cardtest.c $(VSRC) $(CSRC) -lm

.PHONY: prototest cardtest
