/* Host tests of libradio (spec R16.0.1, R16.0.2, R2b.1a; task T30a). */
#include <stdio.h>
#include <string.h>
#include "radio.h"

static int fails;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
		__LINE__, #c); fails++; } } while (0)

static const char light[] =
  "name\tble-light\ntype\tble-light\nversion\t1.23.0\nfus\t2.0.0\n"
  "addr\t0x080D7000\nsize\t100540\nsha256\t"
  "8c1713a8685b764c4c71eeee39629f9fc67accf52e98df7d64bc3b46a385a89a\n"
  "note\tSTM32CubeWB v1.23.0\n";

static int parse(const char *s, struct rd_man *m)
{
  int line;

  return rd_parse(s, strlen(s), m, &line);
}

/* light with key k's line replaced by repl (NULL: removed) */
static int variant(const char *k, const char *repl, struct rd_man *m)
{
  char buf[512], *p, *e;

  strcpy(buf, light);
  p = strstr(buf, k);
  e = strchr(p, '\n') + 1;
  memmove(p + (repl ? strlen(repl) : 0), e, strlen(e) + 1);
  if (repl) memcpy(p, repl, strlen(repl));
  return parse(buf, m);
}

int main(void)
{
  struct rd_man m, m2;
  struct rd_layout l;
  char out[512];
  unsigned long a[3];
  int n;

  /* parsing */
  T(parse(light, &m) == RD_OK);
  T(strcmp(m.name, "ble-light") == 0 && m.addr == 0x080D7000UL);
  T(m.size == 100540 && m.sha[0] == 0x8c && m.sha[31] == 0x9a);
  n = rd_format(&m, out, sizeof(out));
  T(n == (int) strlen(light) && strcmp(out, light) == 0);
  T(parse(out, &m2) == RD_OK && memcmp(&m, &m2, sizeof(m)) == 0);
  T(rd_format(&m, out, 40) == -1);
  T(variant("note", NULL, &m) == RD_OK);			/* optional */
  T(variant("sha256", NULL, &m) == RD_EMISSING);
  T(variant("fus", NULL, &m) == RD_EMISSING);
  T(variant("sha256", "sha256\t8c17\n", &m) == RD_EVALUE);
  T(variant("fus", "fus\t2.0\n", &m) == RD_EVALUE);
  T(variant("fus", "fus\t2.0.x\n", &m) == RD_EVALUE);
  T(variant("addr", "addr\t0x080D7001\n", &m) == RD_ESLOTHIGH);
  T(variant("addr", "addr\t0x09000000\n", &m) == RD_ESLOTHIGH);
  T(variant("addr", "addr\t0x1FFFFFFFF\n", &m) == RD_EVALUE);
  T(variant("size", "size\t0\n", &m) == RD_ESLOTHIGH);
  T(variant("size", "size\t0x100000\n", &m) == RD_ESLOTHIGH);	/* past end */
  T(variant("name", "name\t../etc\n", &m) == RD_ENAME);
  T(variant("name", "name\t-x\n", &m) == RD_ENAME);
  T(variant("name", "colour\tred\n", &m) == RD_EKEY);
  T(variant("name", "name ble\n", &m) == RD_EKEY);
  T(variant("note", "note\tbad\001\n", &m) == RD_EVALUE);
  T(variant("note", "name\tagain\n", &m) == RD_EKEY);		/* twice */
  T(parse("# comment\r\n\r\n", &m) == RD_EMISSING);

  /* versions */
  T(rd_vercmp("1.2.0", "2.0.0") < 0 && rd_vercmp("2.0.1", "2.0.0") > 0);
  T(rd_vercmp("1.10.0", "1.9.9") > 0 && rd_vercmp("1.2.0", "1.2.0") == 0);
  T(rd_vercmp("bad", "1.0.0") < 0);

  /* checks before radio add: bad hash, too-new fus, slot over the base */
  parse(light, &m);
  memcpy(m2.sha, m.sha, 32);
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_OK);
  T(rd_check(&m, m2.sha, 100540, NULL) == RD_OK);
  T(rd_check(&m, m2.sha, 100539, "2.0.0") == RD_ESIZE);
  m2.sha[5] ^= 1;
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_EHASH);
  m2.sha[5] ^= 1;
  T(rd_check(&m, m2.sha, 100540, "1.2.0") == RD_EFUS);
  m.addr = 0x0806B000;			/* BLE + Thread static: fits */
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_OK);
  m.addr = 0x08069000;			/* BLE + Thread dynamic: 28 KB of
					 * cache only, refused */
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_ESLOTLOW);
  m.addr = 0x08048000 + 0xA000 + 0x10000;	/* no 32 KB cache left */
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_ESLOTLOW);
  m.addr = 0x08048000 + 0xA000 + 0x10000 + 0x8000;	/* just enough */
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_OK);
  m.addr = 0x08030000;			/* over libc */
  T(rd_check(&m, m2.sha, 100540, "2.0.0") == RD_ESLOTLOW);

  /* the slot of a set of images */
  a[0] = 0x080D7000, a[1] = 0x080D0000, a[2] = 0x0807F000;
  T(rd_slot(a, 3) == 0x0807F000 && rd_slot(a, 2) == 0x080D0000);
  T(rd_slot(a, 0) == 0);

  /* layout, no slot: as R2b.1 today (stock light stack) */
  T(rd_layout(0x080D7000, 0, &l) == 0);
  T(l.guard == 0x080C7000 && l.cache_end == l.guard);
  T(l.inst == 0x08048000 && l.inst_end == 0x080A7000 && l.cache == l.inst_end);
  /* slot above the cache: no change */
  T(rd_layout(0x080D7000, 0x080D0000, &l) == 0 && l.inst_end == 0x080A7000);
  /* slot for BLE + Zigbee: installs stop there (pkg install --flash),
   * the cache fills the slot while the light stack runs */
  T(rd_layout(0x080D7000, 0x0807F000, &l) == 0);
  T(l.inst_end == 0x0807F000 && l.cache == 0x0807F000
    && l.cache_end == 0x080C7000);
  /* after the swap to BLE + Zigbee: the guard moves under the slot */
  T(rd_layout(0x0807F000, 0x0807F000, &l) == 0);
  T(l.guard == 0x0806F000 && l.cache_end - l.cache == 0x1D000);
  T(l.inst_end == 0x08052000);	/* only the reserved pages are left */
  /* records above the new guard are dead; below, alive */
  T(rd_dead(&l, 0x08070000, 0x1000) && rd_dead(&l, 0x0806E000, 0x2000));
  T(!rd_dead(&l, 0x0806E000, 0x1000));
  /* a big stack: the cache shrinks to what is left, down to 32 KB */
  T(rd_layout(0x08048000 + 0xA000 + 0x10000 + 0x8000, 0, &l) == 0);
  T(l.inst_end == 0x08052000 && l.cache_end - l.cache == 0x8000);
  T(rd_layout(0x08048000 + 0xA000 + 0x10000 + 0x7000, 0, &l) == -1);
  T(rd_layout(0x08200000, 0, &l) == -1);
  /* a slot below the reserved pages never gives them away */
  T(rd_layout(0x080D7000, 0x08049000, &l) == 0 && l.inst_end == 0x08052000);

  for (n = 0; n < RD_NERR; n++) T(rd_err(n)[0] != '?');
  T(rd_err(-1)[0] == '?');
  if (fails) { printf("radiotest: %d failed\n", fails); return 1; }
  printf("radiotest: all passed\n");
  return 0;
}
