/* nfc_supported_cards_list.h - the parsers linked in (#404), one X() per
 * src/lib/nfc/plugins/supported_cards/<name>.c.  The build compiles the
 * same list (flipper/tests/nfc/proto.mk, NSC_PARSERS).  Order is stock's
 * load order (alphabetical); the first parser that parses a card wins. */
#define NFC_SUPPORTED_CARDS(X) \
    X(aime) \
    X(bambu) \
    X(csc) \
    X(gallagher) \
    X(hi) \
    X(hworld) \
    X(metromoney) \
    X(microel) \
    X(mizip) \
    X(myki) \
    X(smartrider) \
    X(two_cities) \
    X(washcity) \
    X(zolotaya_korona_online)
