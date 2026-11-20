/* nfc_supported_cards_list.h - the parsers linked in (#404), one X() per
 * src/lib/nfc/plugins/supported_cards/<name>.c.  The build compiles the
 * same list (flipper/tests/nfc/proto.mk, NSC_PARSERS).  Order is stock's
 * load order (alphabetical); the first parser that parses a card wins. */
#define NFC_SUPPORTED_CARDS(X) \
    X(aime) \
    X(bambu) \
    X(bip) \
    X(charliecard) \
    X(clipper) \
    X(csc) \
    X(gallagher) \
    X(hi) \
    X(hid) \
    X(hworld) \
    X(itso) \
    X(kazan) \
    X(metromoney) \
    X(microel) \
    X(mizip) \
    X(myki) \
    X(opal) \
    X(smartrider) \
    X(social_moscow) \
    X(troika) \
    X(two_cities) \
    X(umarsh) \
    X(washcity) \
    X(zolotaya_korona) \
    X(zolotaya_korona_online) \
    X(all_in_one) \
    X(sonicare) \
    X(trt) \
    X(ventra) \
    X(ndef_ul) \
    X(ndef_mfc) \
    X(aic) \
    X(mykey) \
    X(plantain) \
    X(sev_tk) /* sevppk_tk.c */ \
    X(sk_tk) \
    X(szppk_so) \
    X(skylanders)
