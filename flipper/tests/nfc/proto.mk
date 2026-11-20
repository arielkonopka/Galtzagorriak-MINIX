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
VHDR	+= cm_mfc.h proto.mk
# NfcDevice (vendored nfc_device.c over a reduced table) and the
# supported_cards parsers, linked in through nfc_supported_cards.c.  The
# parser list is src/lib/libnfc/nfc_supported_cards_list.h; NSC_PARSERS
# must name the same files.
NSC_PARSERS := aime bambu bip charliecard clipper csc gallagher \
	       hi hid hworld itso kazan metromoney microel \
	       mizip myki opal smartrider social_moscow troika two_cities \
	       umarsh washcity zolotaya_korona zolotaya_korona_online \
	       all_in_one sonicare trt aic mykey plantain sevppk_tk sk_tk szppk_so skylanders \
	       disney_infinity emv
CSRC	:= $(L)/nfc_device_defs_shim.c $(V)/nfc/nfc_device.c $(V)/nfc/nfc_device_i.c \
	   $(L)/nfc_supported_cards.c $(V)/api/gallagher/gallagher_util.c \
	   $(V)/api/mosgortrans/mosgortrans_util.c $(L)/nfc_shim_time.c $(V)/toolbox/strint.c \
	   $(patsubst %,$(V)/plugins/supported_cards/%.c,$(NSC_PARSERS))
VHDR	+= $(L)/nfc_supported_cards.h $(L)/nfc_supported_cards_list.h
# -Wno-unused-variable: mf_classic_poller.c keeps a few.
# -fno-sanitize=shift-base: stock builds the CUID as uid[0] << 24 (an int
# shift into the sign bit for UIDs from 0x80); GCC defines that shift
# (manual, "Integers implementation"), so only this one check is off.
VWARN	:= -Wno-format -Wno-unused-variable -Wno-unused-function -fno-sanitize=shift-base

prototest: $(B)/prototest
	./$(B)/prototest

$(B)/prototest: prototest.c $(VSRC) $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -o $@ prototest.c $(VSRC) -lm

# ndef.c is built once per protocol (NDEF_PROTO), as stock's
# application.fam does; each copy gets its own entry point name, and the
# Classic copy its own names for ndef.c's three non-static functions.
NDEF_MFC_REN := -Dndef_parse_message=ndef_mfc_parse_message \
	-Dndef_parse_record=ndef_mfc_parse_record -Dndef_parse_tlv=ndef_mfc_parse_tlv
NDEF_OBJ := $(B)/ndef_ul.o $(B)/ndef_mfc.o $(B)/ventra.o $(B)/banapass.o
# banapass: the vendored copy stays byte for byte; its patch
# (src/lib/libnfc/patches/banapass.patch, case labels) is applied to a
# copy in the build directory.
$(B)/banapass.c: $(V)/plugins/supported_cards/banapass.c $(L)/patches/banapass.patch
	@mkdir -p $(B)
	patch -s -o $@ $< $(L)/patches/banapass.patch
$(B)/banapass.o: $(B)/banapass.c $(VHDR)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -I$(V)/plugins/supported_cards -c -o $@ $<
# Stock links each parser as its own plugin; here they share one image,
# so ventra's global time_now() (charliecard has one too) is renamed.
$(B)/ventra.o: $(V)/plugins/supported_cards/ventra.c $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -Dtime_now=ventra_time_now -c -o $@ $<
$(B)/ndef_ul.o: $(V)/plugins/supported_cards/ndef.c $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -DNDEF_PROTO=1 -Dndef_plugin_ep=ndef_ul_plugin_ep -c -o $@ $<
$(B)/ndef_mfc.o: $(V)/plugins/supported_cards/ndef.c $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -DNDEF_PROTO=2 -Dndef_plugin_ep=ndef_mfc_plugin_ep $(NDEF_MFC_REN) -c -o $@ $<

cardtest: $(B)/cardtest
	./$(B)/cardtest

$(B)/cardtest: cardtest.c $(VSRC) $(CSRC) $(VHDR) $(NDEF_OBJ)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -o $@ cardtest.c $(VSRC) $(CSRC) $(NDEF_OBJ) -lm

.PHONY: prototest cardtest
# Part 5 (#412): MIFARE Ultralight/NTAG (poller and sync layer; mbedTLS
# AES/3DES failed closed by nfc_shim_crypto.c), FeliCa, ST25TB, EMV.
P5SRC	:= $(L)/nfc_shim_crypto.c $(L)/mf_ultralight.c $(L)/nfc_sha1.c \
	   $(V)/nfc/protocols/mf_ultralight/mf_ultralight.c \
	   $(V)/nfc/protocols/mf_ultralight/mf_ultralight_aes_crypto.c \
	   $(V)/nfc/protocols/mf_ultralight/mf_ultralight_poller.c \
	   $(V)/nfc/protocols/mf_ultralight/mf_ultralight_poller_i.c \
	   $(V)/nfc/protocols/mf_ultralight/mf_ultralight_poller_sync.c \
	   $(L)/nfc_shim_str.c $(L)/nfc_shim_ff.c $(L)/nfc_emv_parser.c $(V)/nfc/helpers/felica_crc.c $(V)/toolbox/pretty_format.c \
	   $(V)/nfc/protocols/felica/felica.c $(V)/nfc/protocols/felica/felica_i.c \
	   $(V)/nfc/protocols/felica/felica_poller.c $(V)/nfc/protocols/felica/felica_poller_i.c \
	   $(V)/nfc/protocols/felica/felica_poller_sync.c \
	   $(V)/nfc/protocols/st25tb/st25tb.c $(V)/nfc/protocols/st25tb/st25tb_poller.c \
	   $(V)/nfc/protocols/st25tb/st25tb_poller_i.c $(V)/nfc/protocols/st25tb/st25tb_poller_sync.c \
	   $(V)/nfc/protocols/emv/emv.c $(V)/nfc/protocols/emv/emv_poller.c \
	   $(V)/nfc/protocols/emv/emv_poller_i.c
# -fshort-enums: MfUltralightConfigPages has an enum bit-field (mirror_conf)
# in an unpacked inner struct; with 4-byte enums the MIRROR union is 4
# bytes and AUTH0/ACCESS/PWD/PACK land on the wrong bytes.  The struct
# must be 16 bytes (one config page set), checked in p5test.c.
P5FLAGS	:= -fshort-enums
VSRC	+= $(P5SRC)
VHDR	+= cm_mful.h cm_felica.h cm_st25tb.h cm_emv.h $(wildcard $(L)/shim/mlib/*.h) $(wildcard $(L)/shim/mbedtls/*.h $(L)/shim/mbedtls/include/mbedtls/*.h)
# The poller/device tables list Ultralight, so every test links it.
VWARN	+= $(P5FLAGS)

p5test: $(B)/p5test
	./$(B)/p5test

$(B)/p5test: p5test.c $(VSRC) $(VHDR)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(VWARN) $(VINC) -o $@ p5test.c $(VSRC) -lm

.PHONY: p5test
