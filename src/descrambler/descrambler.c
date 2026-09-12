/*
 *  Tvheadend
 *  Copyright (C) 2013 Andreas Öman
 *  Copyright (C) 2014,2015,2016,2017 Jaroslav Kysela
 *
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>

#include "tvheadend.h"
#include "config.h"
#include "settings.h"
#include "descrambler.h"
#include "caid.h"
#include "caclient.h"
#include "input.h"
#include "input/mpegts/tsdemux.h"
#include "dvbcam.h"
#include "streaming.h"

#if 0
#define DEBUG2 1
#define debug2(fmt, ...) tvhtrace(LS_DESCRAMBLER, fmt, ##__VA_ARGS__)
#else
#undef DEBUG2
#define debug2(fmt, ...) do { } while(0)
#endif


#define ECM_PARITY_DEFAULT              0
#define ECM_PARITY_80EVEN_81ODD		1
#define ECM_PARITY_81EVEN_80ODD         2

/*
 * nowosc: maksymalny wiek (w ms) zcache'owanego klucza "zapasowego" od
 * innego klienta CA, ponizej ktorego ecm_reset() moze go uzyc od razu
 * zamiast pelnego resetu. Konfigurowalne per-CAID w pliku "descrambler"
 * (pole "standby_age", w milisekundach) - patrz descrambler_load_hints().
 *
 * bugfix: byl tu wczesniej maly, staly domyslny prog (5000, potem 8000ms).
 * To bylo strukturalnie zle dobrane - klucz zapasowy odswieza sie NAJWYZEJ
 * raz na "interval" (dostaje go tak samo rzadko jak klucz aktywny, przy
 * okazji tego samego ECM), a potrzebny jest dopiero tuz PRZED kolejna
 * rotacja CW, czyli realnie ma wtedy wiek bliski calego "interval".
 * Przy typowych interval >= 10-20s maly staly prog gwarantowal, ze zapas
 * zawsze zdazy "wygasnac" (patrz warunek nizej w ecm_reset()), zanim w
 * ogole dojdzie do awarii aktywnego klienta - failover nigdy nie mial
 * z czego skorzystac i zawsze spadal do pelnego resetu ECM (dlugie
 * wiszenie obrazu). 0 tutaj = "nie ustawiono jawnie", patrz uzycie w
 * descrambler_load_hints()/descrambler_service_start() - domyslnie
 * rownamy standby_age do "interval".
 */
#define ECM_STANDBY_AGE_DEFAULT         0

typedef struct th_descrambler_data {
  TAILQ_ENTRY(th_descrambler_data) dd_link;
  int64_t dd_timestamp;
  sbuf_t dd_sbuf;
  th_descrambler_key_t *dd_key;
  uint8_t dd_key_changed;
} th_descrambler_data_t;

typedef struct th_descrambler_hint {
  TAILQ_ENTRY(th_descrambler_hint) dh_link;
  uint16_t dh_caid;
  uint16_t dh_mask;
  uint32_t dh_interval;
  uint32_t dh_paritycheck;
  uint32_t dh_ecmparity;
  uint32_t dh_standby_age; /* nowosc: patrz ecm_reset()/descrambler_keys() */
  uint32_t dh_constcw: 1;
  uint32_t dh_quickecm: 1;
  uint32_t dh_multipid: 1;
} th_descrambler_hint_t;

/*
 * nowosc (nowa #2 - routing czytnikow CA per CAID): opcjonalne reguly z
 * "data/conf/descrambler_routing" ograniczajace, ktore skonfigurowane
 * klienty CA (po ich nazwie/tytule) wolno w ogole uruchomic dla uslugi
 * niosacej dany CAID. Bez tego pliku (albo pustego) - zero zmiany
 * zachowania, tak jak dzisiaj: caclient_start() odpytuje WSZYSTKICH
 * wlaczonych klientow dla kazdej uslugi, co przy kilku serwerach
 * card-sharingu z limitem "1 rownolegly strumien" powoduje kolizje
 * ("Already has a key for service" / NOK).
 */
typedef struct th_descrambler_route {
  TAILQ_ENTRY(th_descrambler_route) dt_link;
  uint16_t dt_caid;
  uint16_t dt_mask;
  char    *dt_allow;   /* lista nazw klientow (po przecinku) - jesli ustawiona,
                           TYLKO wymienieni startuja dla tego CAID */
  char    *dt_deny;    /* lista nazw klientow do wykluczenia; brana pod uwage
                           tylko gdy dt_allow jest puste */
} th_descrambler_route_t;

static TAILQ_HEAD( , th_descrambler_route) ca_routes;

TAILQ_HEAD(th_descrambler_queue, th_descrambler_data);
static TAILQ_HEAD( , th_descrambler_hint) ca_hints;

static int ca_hints_quickecm;

/*
 * nowosc (#2 - hot-reload podpowiedzi CA):
 *
 * Do tej pory plik "data/conf/descrambler" byl wczytywany DOKLADNIE RAZ w
 * descrambler_init() i lista ca_hints traktowana jako niezmienna do konca
 * zycia procesu - kazda zmiana interval/paritycheck/quickecm/... wymagala
 * pelnego restartu TVH. Teraz plik jest pilnowany (mtime) przez lekki
 * mtimer i przeladowywany "w locie". Nowe wartosci dotycza uslug
 * uruchamianych PO przeladowaniu (biezace strumienie maja juz skopiowane
 * wartosci w swoim th_descrambler_runtime_t - trzeba je przestartowac,
 * np. przełączając kanał).
 *
 * ca_hints_mutex serializuje odczyt listy (descrambler_service_start,
 * descrambler_quick_ecm) z jej podmiana (descrambler_reload_hints).
 */
static tvh_mutex_t ca_hints_mutex;
static int64_t     ca_hints_file_mtime;
static mtimer_t    ca_hints_reload_timer;

/*
 * nowosc (#1 - wyscig ECM / "ciepli" czytnicy):
 *
 * Domyslnie (0) zachowanie bez zmian: pierwszy klient CA, ktory dostarczy
 * klucz, zostaje aktywny, a pozostali przechodza w DS_IDLE (td_ecm_idle -
 * przestaja przetwarzac ECM). Gdy jeden serwer sie zapcha, failover
 * korzysta tylko z JEDNEGO, byc moze juz przeterminowanego, cache'u
 * standby.
 *
 * Wlaczone (1): pozostali czytnicy NIE ida w idle - dalej podazaja za
 * zmianami ECM i na biezaco odswiezaja swoj klucz standby (td_standby_*).
 * Dzieki temu w chwili awarii aktywnego czytnika failover ma zawsze
 * swiezy zapas z innego, niezaleznego serwera. Koszt: N-krotny ruch ECM
 * do serwerow kart - niektore konta CCcam maja limit 1 rownoleglego
 * strumienia i beda zwracac NOK ("Already has a key for service").
 * Dlatego opcja jest domyslnie wylaczona i wlaczana swiadomie.
 */
#define ecm_race_enabled() (config.descrambler_ecm_race)

/*
 *
 */
static inline int extractpid(const uint8_t *tsb)
{
  return (tsb[1] & 0x1f) << 8 | tsb[2];
}

#if DEBUG2
static inline const char *keystr(const uint8_t *tsb)
{
  uint8_t b = tsb[3];
  if (b & 0x80)
    return (b & 0x40) ? "odd" : "even";
  return (b & 0x40) ? "none2" : "none";
}
#endif

/*
 *
 */
static void
descrambler_data_destroy(th_descrambler_runtime_t *dr, th_descrambler_data_t *dd, int skip)
{
  if (dd) {
    if (skip && dr->dr_skip)
      ts_skip_packet2((mpegts_service_t *)dr->dr_service,
                      dd->dd_sbuf.sb_data, dd->dd_sbuf.sb_ptr);
    dr->dr_queue_total -= dd->dd_sbuf.sb_ptr;
    TAILQ_REMOVE(&dr->dr_queue, dd, dd_link);
    sbuf_free(&dd->dd_sbuf);
    free(dd);
#if ENABLE_TRACE
    if (TAILQ_EMPTY(&dr->dr_queue))
      assert(dr->dr_queue_total == 0);
#endif
  }
}

static void
descrambler_data_append(th_descrambler_runtime_t *dr, const uint8_t *tsb, int len)
{
  th_descrambler_data_t *dd;
  const uint8_t *tsb0;
  uint16_t pid1, pid2;

  if (len == 0)
    return;
  dd = TAILQ_LAST(&dr->dr_queue, th_descrambler_queue);
  if (dd && (tsb0 = dd->dd_sbuf.sb_data) != NULL) {
    if (dr->dr_key_multipid) {
      pid1 = extractpid(tsb0);
      pid2 = extractpid(tsb);
    } else {
      pid1 = pid2 = 0;
    }
    if (dd && monocmpfastsec(dd->dd_timestamp, mclk()) &&
        (tsb0[3] & 0xc0) == (tsb[3] & 0xc0) && /* key match */
        pid1 == pid2) {
      debug2("%p: data append %d, timestamp %ld, %s[%d]", dr, len, dd->dd_timestamp, keystr(tsb0), extractpid(tsb0));
      sbuf_append(&dd->dd_sbuf, tsb, len);
      dr->dr_queue_total += len;
      return;
    }
  }
  dd = malloc(sizeof(*dd));
  dd->dd_key = NULL;
  dd->dd_timestamp = mclk();
  debug2("%p: data append2 %d, timestamp %ld, %s[%d]", dr, len, dd->dd_timestamp, keystr(tsb), extractpid(tsb));
  sbuf_init(&dd->dd_sbuf);
  sbuf_append(&dd->dd_sbuf, tsb, len);
  TAILQ_INSERT_TAIL(&dr->dr_queue, dd, dd_link);
  dr->dr_queue_total += len;
}

static void
descrambler_data_add_key(th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, int change, int head)
{
  th_descrambler_data_t *dd;

  dd = calloc(1, sizeof(*dd));
  dd->dd_timestamp = mclk();
  dd->dd_key = tk;
  dd->dd_key_changed = change;
  debug2("%p: data %s key %d, timestamp %ld", dr, head ? "insert" : "append", tk->key_pid, dd->dd_timestamp);
  if (head)
    TAILQ_INSERT_HEAD(&dr->dr_queue, dd, dd_link);
  else
    TAILQ_INSERT_TAIL(&dr->dr_queue, dd, dd_link);
}

static void
descrambler_data_cut(th_descrambler_runtime_t *dr, int len)
{
  th_descrambler_data_t *dd;

  while (len > 0) {
    TAILQ_FOREACH(dd, &dr->dr_queue, dd_link)
      if (dd && dd->dd_sbuf.sb_data) break;
    if (dd == NULL) return;
    if (dr->dr_skip)
      ts_skip_packet2((mpegts_service_t *)dr->dr_service,
                      dd->dd_sbuf.sb_data, MIN(len, dd->dd_sbuf.sb_ptr));
    if (len < dd->dd_sbuf.sb_ptr) {
      sbuf_cut(&dd->dd_sbuf, len);
      dr->dr_queue_total -= len;
      break;
    }
    len -= dd->dd_sbuf.sb_ptr;
    descrambler_data_destroy(dr, dd, 1);
  }
}

static int
descrambler_data_key_check(th_descrambler_runtime_t *dr, uint8_t key, int len)
{
  th_descrambler_data_t *dd;
  int off = 0, l;
  uint_fast8_t ki;

  if ((dd = TAILQ_FIRST(&dr->dr_queue)) == NULL)
    return len;
  while (len > 0) {
    while (dd && dd->dd_sbuf.sb_data == NULL)
      dd = TAILQ_NEXT(dd, dd_link);
    if (dd == NULL) break;
    l = dd->dd_sbuf.sb_ptr;
    for (off = 0; off < l && len > 0; off += 188, l -= 188) {
      ki = dd->dd_sbuf.sb_data[off + 3];
      if (ki == 0) continue;
      if ((ki & 0xc0) != key) return -1;
      len -= 188;
    }
    dd = TAILQ_NEXT(dd, dd_link);
  }
  return len;
}

static int
descrambler_data_analyze(th_descrambler_runtime_t *dr,
                         th_descrambler_data_t *dd, uint8_t ki)
{
  th_descrambler_data_t *dd2;
  const uint8_t *tsb0;
  int packets = 0, blocks = 0;

  for (dd2 = TAILQ_NEXT(dd, dd_link); dd2; dd2 = TAILQ_NEXT(dd2, dd_link)) {
    /*
     * bugfix: bylo "dd->dd_sbuf...." (stary, niezmieniajacy sie wskaznik
     * przekazany do funkcji) zamiast "dd2->dd_sbuf...." (biezacy element
     * petli). W praktyce funkcja sprawdzala od nowa wciaz ten sam,
     * pierwszy bajt zamiast bajtu KAZDEGO kolejnego bloku - warunek byl
     * wiec stale prawdziwy lub stale falszywy przez cala petle, przez co
     * "paritycheck" nie weryfikowal realnie ciaglosci nowej parzystosci
     * w kolejnych blokach, tylko liczyl bajty bez faktycznej kontroli.
     */
    tsb0 = dd2->dd_sbuf.sb_data;
    if (tsb0 == NULL || dd2->dd_sbuf.sb_ptr == 0) continue;
    if ((tsb0[3] & 0x80) != 0 && (tsb0[3] & 0x40) == (ki & 0x40)) {
      packets += dd2->dd_sbuf.sb_ptr;
      if (packets >= dr->dr_paritycheck)
        return 1;
    } else {
      packets = 0;
    }
    if (++blocks > 10)
      return 2; /* process packets, no key change */
  }
  return 0;
}

/*
 *
 */
static inline void
descrambler_ecmsec_unref(descrambler_ecmsec_t *des)
{
  int v = atomic_dec(&des->refcnt, 1);
  assert(v > 0);
  if (v == 1) {
    free(des->last_data);
    free(des);
  }
}

static void
descrambler_destroy_ecmsec(descrambler_ecmsec_t *des)
{
  LIST_REMOVE(des, link);
  descrambler_ecmsec_unref(des);
}

static void
descrambler_destroy_all_ecmsecs(descrambler_section_t *ds)
{
  descrambler_ecmsec_t *des;
  while ((des = LIST_FIRST(&ds->ecmsecs)) != NULL)
    descrambler_destroy_ecmsec(des);
}

static void
descrambler_destroy_section ( descrambler_section_t *ds, int emm )
{
  ds->callback(ds->opaque, -1, NULL, 0, emm);
  descrambler_destroy_all_ecmsecs(ds);
  free(ds);
}

static void
descrambler_destroy_table_( descrambler_table_t *dt )
{
  mpegts_table_destroy(dt->table);
  free(dt);
}

static void
descrambler_destroy_table( descrambler_table_t *dt, int emm )
{
  descrambler_section_t *ds;
  while ((ds = TAILQ_FIRST(&dt->sections)) != NULL) {
    TAILQ_REMOVE(&dt->sections, ds, link);
    descrambler_destroy_section(ds, emm);
  }
  descrambler_destroy_table_(dt);
}

/*
 *
 */
static struct strtab ecmparitytab[] = {
  { "default",  ECM_PARITY_DEFAULT },
  { "standard", ECM_PARITY_80EVEN_81ODD },
  { "inverted", ECM_PARITY_81EVEN_80ODD },
};

/*
 *
 */
static void
descrambler_load_hints(htsmsg_t *m)
{
  th_descrambler_hint_t hint, *dhint;
  htsmsg_t *e;
  htsmsg_field_t *f;
  const char *s;
  char sabuf[24];

  HTSMSG_FOREACH(f, m) {
    if (!(e = htsmsg_field_get_map(f))) continue;
    if ((s = htsmsg_get_str(e, "caid")) == NULL) continue;
    memset(&hint, 0, sizeof(hint));
    hint.dh_caid = strtol(s, NULL, 16);
    hint.dh_mask = 0xffff;
    if ((s = htsmsg_get_str(e, "mask")) != NULL)
      hint.dh_mask = strtol(s, NULL, 16);
    hint.dh_constcw = htsmsg_get_bool_or_default(e, "constcw", 0);
    hint.dh_quickecm = htsmsg_get_bool_or_default(e, "quickecm", 0);
    hint.dh_multipid = htsmsg_get_bool_or_default(e, "multipid", 0);
    hint.dh_interval = htsmsg_get_s32_or_default(e, "interval", 10000);
    hint.dh_paritycheck = htsmsg_get_s32_or_default(e, "paritycheck", 20);
    hint.dh_ecmparity = str2val_def(htsmsg_get_str(e, "ecmparity"), ecmparitytab, ECM_PARITY_DEFAULT);
    hint.dh_standby_age = htsmsg_get_s32_or_default(e, "standby_age", ECM_STANDBY_AGE_DEFAULT);
    if (hint.dh_standby_age)
      snprintf(sabuf, sizeof(sabuf), "%ums", hint.dh_standby_age);
    else
      snprintf(sabuf, sizeof(sabuf), "auto(=interval)");
    tvhinfo(LS_DESCRAMBLER, "adding CAID %04X/%04X as%s%s%s interval %ums pc %d ep %s sa %s (%s)",
                            hint.dh_caid, hint.dh_mask,
                            hint.dh_constcw ? " ConstCW" : "",
                            hint.dh_quickecm ? " QuickECM" : "",
                            hint.dh_multipid ? " MultiPID" : "",
                            hint.dh_interval,
                            hint.dh_paritycheck,
                            val2str(hint.dh_ecmparity, ecmparitytab),
                            sabuf,
                            htsmsg_get_str(e, "name") ?: "unknown");
    dhint = malloc(sizeof(*dhint));
    *dhint = hint;
    TAILQ_INSERT_TAIL(&ca_hints, dhint, dh_link);
    if (hint.dh_quickecm) ca_hints_quickecm++;
  }
}

/*
 * nowosc (nowa #2): wczytaj reguly routingu czytnikow z listy "route" w
 * "data/conf/descrambler_routing". Format zblizony do podpowiedzi CAID:
 *   { "caid": "1884", "mask": "FFFF", "allow": "testowy,s2.skyhd1" }
 *   { "caid": "1861", "mask": "FFFF", "deny": "s1.skyhd1" }
 * "allow" - lista nazw (tytulow) klientow CA po przecinku; jesli podana,
 *           TYLKO oni startuja dla uslug niosacych ten CAID.
 * "deny"  - jak wyzej, ale wykluczajaco; brana pod uwage tylko gdy "allow"
 *           nie jest ustawione dla danej reguly.
 * Zob. descrambler_client_allowed().
 */
static void
descrambler_load_routes(htsmsg_t *m)
{
  th_descrambler_route_t *rt;
  htsmsg_t *e;
  htsmsg_field_t *f;
  const char *s, *allow, *deny;

  HTSMSG_FOREACH(f, m) {
    if (!(e = htsmsg_field_get_map(f))) continue;
    if ((s = htsmsg_get_str(e, "caid")) == NULL) continue;
    allow = htsmsg_get_str(e, "allow");
    deny  = htsmsg_get_str(e, "deny");
    if (!allow && !deny) continue;
    rt = calloc(1, sizeof(*rt));
    rt->dt_caid = strtol(s, NULL, 16);
    rt->dt_mask = 0xffff;
    if ((s = htsmsg_get_str(e, "mask")) != NULL)
      rt->dt_mask = strtol(s, NULL, 16);
    if (allow) rt->dt_allow = strdup(allow);
    if (deny)  rt->dt_deny  = strdup(deny);
    tvhinfo(LS_DESCRAMBLER, "adding CA route for CAID %04X/%04X: %s=%s",
            rt->dt_caid, rt->dt_mask,
            allow ? "allow" : "deny", allow ?: deny);
    TAILQ_INSERT_TAIL(&ca_routes, rt, dt_link);
  }
}

/*
 * nowosc (#2 / nowa #2): oproznij podpowiedzi CAID i reguly routingu.
 * Wolane pod ca_hints_mutex.
 */
static void
descrambler_clear_hints ( void )
{
  th_descrambler_hint_t *hint;
  th_descrambler_route_t *rt;

  while ((hint = TAILQ_FIRST(&ca_hints)) != NULL) {
    TAILQ_REMOVE(&ca_hints, hint, dh_link);
    free(hint);
  }
  ca_hints_quickecm = 0;
  while ((rt = TAILQ_FIRST(&ca_routes)) != NULL) {
    TAILQ_REMOVE(&ca_routes, rt, dt_link);
    free(rt->dt_allow);
    free(rt->dt_deny);
    free(rt);
  }
}

/*
 * nowosc (#2 / nowa #2): najnowsze mtime spomiedzy plikow "descrambler" i
 * "descrambler_routing" (sciezka runtime i wbudowana "data/conf/..." -
 * hts_settings_load() probuje obu). Zwraca 0, gdy zaden plik nie istnieje
 * na dysku (np. tylko wersja wkompilowana w filebundle).
 */
static int64_t
descrambler_hints_mtime ( void )
{
  static const char *names[] = { "descrambler", "descrambler_routing" };
  char path[PATH_MAX];
  struct stat st;
  int64_t mt = 0;
  unsigned i;

  for (i = 0; i < ARRAY_SIZE(names); i++) {
    if (hts_settings_buildpath(path, sizeof(path), "%s", names[i]) == 0 &&
        stat(path, &st) == 0 && (int64_t)st.st_mtime > mt)
      mt = (int64_t)st.st_mtime;
    snprintf(path, sizeof(path), "data/conf/%s", names[i]);
    if (stat(path, &st) == 0 && (int64_t)st.st_mtime > mt)
      mt = (int64_t)st.st_mtime;
  }
  return mt;
}

/*
 * nowosc (#2 / nowa #2): (prze)laduj obie listy z plikow. initial=1 przy
 * starcie (cicho), initial=0 przy hot-reload (z logiem i wyczyszczeniem
 * starych list).
 */
static void
descrambler_reload_hints ( int initial )
{
  htsmsg_t *c, *m;

  tvh_mutex_lock(&ca_hints_mutex);
  if (!initial) {
    tvhinfo(LS_DESCRAMBLER, "reloading CA hints/routing (config file changed)");
    descrambler_clear_hints();
  }
  if ((c = hts_settings_load("descrambler")) != NULL) {
    if ((m = htsmsg_get_list(c, "caid")) != NULL)
      descrambler_load_hints(m);
    htsmsg_destroy(c);
  }
  if ((c = hts_settings_load("descrambler_routing")) != NULL) {
    if ((m = htsmsg_get_list(c, "route")) != NULL)
      descrambler_load_routes(m);
    htsmsg_destroy(c);
  }
  ca_hints_file_mtime = descrambler_hints_mtime();
  tvh_mutex_unlock(&ca_hints_mutex);
}

/*
 * nowosc (#2): lekki dozorca - co 15s sprawdza mtime i przeladowuje.
 */
static void
descrambler_hints_reload_cb ( void *aux )
{
  if (tvheadend_is_running()) {
    int64_t mt = descrambler_hints_mtime();
    if (mt && mt != ca_hints_file_mtime)
      descrambler_reload_hints(0);
    mtimer_arm_rel(&ca_hints_reload_timer, descrambler_hints_reload_cb, NULL,
                   sec2mono(15));
  }
}

/*
 *
 */
void
descrambler_init ( void )
{
  tvh_mutex_init(&ca_hints_mutex, NULL);
  TAILQ_INIT(&ca_hints);
  TAILQ_INIT(&ca_routes);
  ca_hints_quickecm = 0;

  caclient_init();

  descrambler_reload_hints(1);
  mtimer_arm_rel(&ca_hints_reload_timer, descrambler_hints_reload_cb, NULL,
                 sec2mono(15));
}

void
descrambler_done ( void )
{
  /*
   * bugfix: mtimer_disarm() wymaga trzymania global_lock ("the
   * global_lock must be held" - main.c) - asercja lock_assert()
   * wewnatrz niego wywoluje abort() gdy lock nie jest trzymany. main()
   * wola descrambler_done() w sekwencji zamykania BEZ global_lock (patrz
   * sasiednie wywolania w main.c - global_lock jest tam trzymany tylko
   * lokalnie wokol epg_save()/timeshift_term()) - bez tej blokady KAZDE
   * czyste zatrzymanie/restart tvheadend konczylo sie SIGABRT zamiast
   * normalnego wylaczenia (patrz ca_hints_reload_timer, dodany razem z
   * timerem "hint hot-reload" - ten blad istnial od tamtej zmiany, po
   * prostu nikt wczesniej nie zatrzymal czysto zbudowanej binarki, zeby
   * to zobaczyc).
   */
  tvh_mutex_lock(&global_lock);
  mtimer_disarm(&ca_hints_reload_timer);
  tvh_mutex_unlock(&global_lock);
  caclient_done();
  tvh_mutex_lock(&ca_hints_mutex);
  descrambler_clear_hints();
  tvh_mutex_unlock(&ca_hints_mutex);
}

/*
 * Decide, if we should work in "quick ECM" mode
 */
static int
descrambler_quick_ecm ( mpegts_service_t *t, int pid )
{
  elementary_stream_t *st;
  th_descrambler_hint_t *hint;
  caid_t *ca;
  int r = 0;

  if (!ca_hints_quickecm)
    return 0;
  tvh_mutex_lock(&ca_hints_mutex);       /* nowosc (#2): lista moze byc przeladowana */
  TAILQ_FOREACH(st, &t->s_components.set_filter, es_filter_link) {
    if (st->es_pid != pid) continue;
    TAILQ_FOREACH(hint, &ca_hints, dh_link) {
      if (!hint->dh_quickecm) continue;
      LIST_FOREACH(ca, &st->es_caids, link) {
        if (ca->use == 0) continue;
        if (hint->dh_caid == (ca->caid & hint->dh_mask)) {
          r = 1;
          goto out;
        }
      }
    }
  }
out:
  tvh_mutex_unlock(&ca_hints_mutex);
  return r;
}

/*
 * nowosc (nowa #2): czy "name" wystepuje na liscie rozdzielonej przecinkami
 * (bez rozroznienia wielkosci liter, biale znaki wokol elementow ignorowane)?
 */
static int
namelist_contains ( const char *list, const char *name )
{
  const char *p = list, *comma;
  size_t namelen = strlen(name), n;

  if (list == NULL || name == NULL)
    return 0;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) break;
    comma = strchr(p, ',');
    n = comma ? (size_t)(comma - p) : strlen(p);
    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t')) n--;
    if (n == namelen && strncasecmp(p, name, n) == 0)
      return 1;
    p += comma ? (size_t)(comma - p) + 1 : n;
  }
  return 0;
}

/*
 * nowosc (nowa #2 - routing czytnikow CA): czy klient CA o nazwie
 * "cac_name" ma w ogole prawo wystartowac dla tej uslugi? Wolane z
 * caclient_start() PRZED wywolaniem cac->cac_start() dla kazdego
 * wlaczonego klienta - to jedyne miejsce wspolne dla wszystkich rodzin
 * klientow (cwc/cccam/capmt/capmt2/dvbcam), wiec filtrowanie tutaj
 * dziala bez zadnych zmian w samych klientach.
 *
 * Semantyka (patrz tez komentarz przy th_descrambler_route_t): brak
 * pliku/regul pasujacych do CAID-ow tej uslugi = zezwol (zero zmiany
 * zachowania wzgledem stanu sprzed tej funkcji). Jesli KTORAKOLWIEK
 * pasujaca regula ma "allow", klient musi byc na liscie choc jednej
 * takiej reguly. W przeciwnym razie (same reguly "deny") - odrzucany
 * jest tylko klient wymieniony w "deny".
 */
int
descrambler_client_allowed ( service_t *t, const char *cac_name )
{
  th_descrambler_route_t *rt;
  elementary_stream_t *st;
  caid_t *ca;
  int have_allow_rule = 0, allowed_explicitly = 0, denied = 0;

  if (TAILQ_EMPTY(&ca_routes) || cac_name == NULL || *cac_name == '\0')
    return 1;

  tvh_mutex_lock(&ca_hints_mutex);

#define CHECK_ROUTE(caidval) \
  TAILQ_FOREACH(rt, &ca_routes, dt_link) { \
    if (rt->dt_caid != ((caidval) & rt->dt_mask)) continue; \
    if (rt->dt_allow) { \
      have_allow_rule = 1; \
      if (namelist_contains(rt->dt_allow, cac_name)) \
        allowed_explicitly = 1; \
    } else if (rt->dt_deny && namelist_contains(rt->dt_deny, cac_name)) { \
      denied = 1; \
    } \
  }

  if (t->s_dvb_forcecaid) {
    CHECK_ROUTE(t->s_dvb_forcecaid);
  } else {
    TAILQ_FOREACH(st, &t->s_components.set_filter, es_filter_link)
      LIST_FOREACH(ca, &st->es_caids, link) {
        if (ca->use == 0) continue;
        CHECK_ROUTE(ca->caid);
      }
  }
#undef CHECK_ROUTE

  tvh_mutex_unlock(&ca_hints_mutex);

  if (have_allow_rule)
    return allowed_explicitly;
  return !denied;
}

/*
 * This routine is called from two places
 * a) start a new service
 * b) restart a running service with possible caid changes
 */
void
descrambler_service_start ( service_t *t )
{
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  th_descrambler_hint_t *hint;
  elementary_stream_t *st;
  caid_t *ca;
  int i, count = 0, constcw = 0, multipid = 0, interval = 10000, paritycheck = 20;
  int ecmparity = ECM_PARITY_DEFAULT;
  int standby_age = ECM_STANDBY_AGE_DEFAULT;

  if (t->s_scrambled_pass)
    return;

  /* nowosc (#2): ca_hints moze byc w tym momencie przeladowywane */
  tvh_mutex_lock(&ca_hints_mutex);
  if (!t->s_dvb_forcecaid) {

    TAILQ_FOREACH(st, &t->s_components.set_filter, es_filter_link)
      LIST_FOREACH(ca, &st->es_caids, link) {
        if (ca->use == 0) continue;
        TAILQ_FOREACH(hint, &ca_hints, dh_link) {
          if (hint->dh_caid == (ca->caid & hint->dh_mask)) {
            if (hint->dh_constcw) constcw = 1;
            if (hint->dh_multipid) multipid = 1;
            if (hint->dh_interval) interval = hint->dh_interval;
            if (hint->dh_paritycheck) paritycheck = hint->dh_paritycheck;
            if (hint->dh_ecmparity != ECM_PARITY_DEFAULT)
              ecmparity = hint->dh_ecmparity;
            if (hint->dh_standby_age) standby_age = hint->dh_standby_age;
          }
        }
        count++;
      }

  } else {

    TAILQ_FOREACH(hint, &ca_hints, dh_link) {
      if (hint->dh_caid == (t->s_dvb_forcecaid & hint->dh_mask)) {
        if (hint->dh_constcw) constcw = 1;
        if (hint->dh_multipid) multipid = 1;
        if (hint->dh_interval) interval = hint->dh_interval;
        if (hint->dh_standby_age) standby_age = hint->dh_standby_age;
      }
    }

  }
  tvh_mutex_unlock(&ca_hints_mutex);

  /* Do not run descrambler on FTA channels */
  if (!t->s_dvb_forcecaid && count == 0)
    return;

  /*
   * bugfix: brak jawnego standby_age per-CAID -> domyslnie rownamy go do
   * "interval" (patrz komentarz przy ECM_STANDBY_AGE_DEFAULT) zamiast
   * malego, stalego progu w sekundach - inaczej klucz zapasowy prawie
   * zawsze byl juz "za stary" w momencie, gdy failover faktycznie go
   * potrzebowal.
   */
  if (!standby_age)
    standby_age = interval;

  tvh_mutex_lock(&t->s_stream_mutex);
  ((mpegts_service_t *)t)->s_dvb_mux->mm_descrambler_flush = 0;
  if (t->s_descramble == NULL) {
    t->s_descramble = dr = calloc(1, sizeof(th_descrambler_runtime_t));
    dr->dr_service = t;
    TAILQ_INIT(&dr->dr_queue);
    for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
      tk = &dr->dr_keys[i];
      tk->key_index = 0xff;
      tk->key_interval = tk->key_initial_interval = ms2mono(interval);
      tvhcsa_init(&tk->key_csa);
      if (!multipid) break;
    }
    dr->dr_paritycheck = MINMAX(paritycheck, 1, 200) * 188;
    dr->dr_initial_paritycheck = MINMAX(paritycheck, 4, 200) * 188;
    dr->dr_ecm_key_margin = ms2mono(interval) / 5;
    dr->dr_ecm_standby_age = ms2mono(standby_age);
    dr->dr_key_const = constcw;
    dr->dr_key_multipid = multipid;
    dr->dr_ecm_parity = ecmparity ?: ECM_PARITY_80EVEN_81ODD;
    if (constcw)
      tvhtrace(LS_DESCRAMBLER, "using constcw for \"%s\"", t->s_nicename);
    if (multipid)
      tvhtrace(LS_DESCRAMBLER, "using multipid for \"%s\"", t->s_nicename);
    dr->dr_skip = 0;
    dr->dr_force_skip = 0;
    if (t->s_dvb_forcecaid == 0xffff)
      dr->dr_descramble = descrambler_pass;
  }
  tvh_mutex_unlock(&t->s_stream_mutex);

  if (t->s_dvb_forcecaid != 0xffff)
    caclient_start(t);
}

void
descrambler_service_stop ( service_t *t )
{
  th_descrambler_t *td;
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  th_descrambler_data_t *dd;
  void *p;
  int i;

  while ((td = LIST_FIRST(&t->s_descramblers)) != NULL)
    td->td_stop(td);
  tvh_mutex_lock(&t->s_stream_mutex);
  dr = t->s_descramble;
  t->s_descramble = NULL;
  t->s_descrambler = NULL;
  p = t->s_descramble_info;
  t->s_descramble_info = NULL;
  tvh_mutex_unlock(&t->s_stream_mutex);
  free(p);
  if (dr) {
    for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
      tk = &dr->dr_keys[i];
      tvhcsa_destroy(&tk->key_csa);
      if (!dr->dr_key_multipid) break;
    }
    while ((dd = TAILQ_FIRST(&dr->dr_queue)) != NULL)
      descrambler_data_destroy(dr, dd, 0);
    free(dr);
  }
}

void
descrambler_caid_changed ( service_t *t )
{
  th_descrambler_t *td;

  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    if (td->td_caid_change)
      td->td_caid_change(td);
  }
}

static void
descrambler_notify_deliver( mpegts_service_t *t, descramble_info_t *di )
{
  streaming_message_t *sm;
  int r;

  lock_assert(&t->s_stream_mutex);
  if (!t->s_descramble_info) {
    t->s_descramble_info = calloc(1, sizeof(*di));
  } else {
    r = memcmp(t->s_descramble_info, di, sizeof(*di));
    if (r == 0) { /* identical */
      free(di);
      return;
    }
  }
  memcpy(t->s_descramble_info, di, sizeof(*di));

  sm = streaming_msg_create_data(SMT_DESCRAMBLE_INFO, di);
  streaming_service_deliver((service_t *)t, sm);
}

/* it's called inside s_stream_mutex lock! */
static void
descrambler_notify_nokey( th_descrambler_runtime_t *dr )
{
  mpegts_service_t *t = (mpegts_service_t *)dr->dr_service;
  descramble_info_t *di;

  tvhdebug(LS_DESCRAMBLER, "no key for service='%s'", t->s_dvb_svcname);

  di = calloc(1, sizeof(*di));

  di->pid = t->s_components.set_pmt_pid;
  descrambler_notify_deliver(t, di);
}

/*
 * nowosc (#3): jednolinijkowe podsumowanie metryk wszystkich czytnikow
 * danej uslugi. Wolane przy parkowaniu czytnika i co kilkanascie ECM.
 * why - krotki powod (np. "periodic", "parked").
 */
static void
descrambler_reader_stats_dump ( service_t *t, const char *why )
{
  th_descrambler_t *td;

  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    if (td->td_ecm_count == 0 && td->td_ecm_nok == 0)
      continue;
    tvhinfo(LS_DESCRAMBLER,
            "reader stats (%s) \"%s\": %s ok=%u nok=%u ecmtime min/avg/max/last=%u/%u/%u/%u ms%s",
            why, t->s_nicename, td->td_nicename,
            td->td_ecm_count, td->td_ecm_nok,
            td->td_ecm_time_min,
            td->td_ecm_count ? (uint32_t)(td->td_ecm_time_sum / td->td_ecm_count) : 0,
            td->td_ecm_time_max, td->td_ecm_time_last,
            td->td_keystate == DS_RESOLVED ? " [active]" :
              (td->td_keystate == DS_FORBIDDEN ? " [denied]" : ""));
  }
}

/*
 * nowosc (#3): zapisz jedna udana odpowiedz ECM dla czytnika td.
 * Wolane pod t->s_stream_mutex.
 */
static void
descrambler_reader_stat_ecm ( th_descrambler_t *td, uint32_t ecmtime )
{
  if (td->td_ecm_time_min == 0 || ecmtime < td->td_ecm_time_min)
    td->td_ecm_time_min = ecmtime;
  if (ecmtime > td->td_ecm_time_max)
    td->td_ecm_time_max = ecmtime;
  td->td_ecm_time_last = ecmtime;
  td->td_ecm_time_sum += ecmtime;
  td->td_ecm_count++;
  /* nowosc: udana odpowiedz - wyczysc powod ostatniego bledu, zeby
     td_ecm_last_error pokazywal TYLKO biezacy/ostatni realny problem,
     nie stary blad sprzed odzyskania */
  td->td_ecm_last_error[0] = '\0';
}

#define ECM_RACE_SWITCH_MIN_MARGIN_MS 20   /* pomin czysty szum pomiaru */
#define ECM_RACE_SWITCH_COOLDOWN      3    /* sekundy - patrz komentarz nizej */

/*
 * nowosc: "pierwszy wygrywa i zostaje aktywny na zawsze" bylo
 * strukturalnie niepelne wzgledem oryginalnej prosby "zawsze odpytuj
 * wszystkich i wybieraj najszybszego" - dotychczas dotyczylo to tylko
 * PIERWSZEGO rozstrzygniecia (albo failovera po awarii), nigdy pozniejszej,
 * biezacej zmiany "kto akurat odpowiada szybciej". Przy wlaczonym ECM
 * race (config.descrambler_ecm_race) "cieple" czytniki standby caly czas
 * odpytuja wlasne serwery na kazdym cyklu ECM - ta funkcja przelacza na
 * kogos, kto ZA OSTATNIM razem odpowiedzial szybciej niz aktywny, czyli
 * najbardziej doslowne "wybierz szybszego TERAZ" (porownanie
 * td_ecm_time_last, nie sredniej z historii - to bylaby ospala
 * reakcja).
 *
 * Jedyna ochrona przed "fruwaniem" miedzy czytnikami o niemal identycznej
 * szybkosci (gdzie kto "wygrywa" zalezaloby od zwyklego szumu sieci) to
 * mala, bezwzgledna granica (20ms) i krotki cooldown
 * (dr_last_speed_switch, 3s) - wystarczajaco krotki, zeby nie
 * przeszkadzac normalnej rotacji CW (typowy interval to >=10s), ale
 * chroniacy przed przelaczaniem kilka razy w ciagu jednego cyklu, gdyby
 * kilku czytnikow odpowiedzialo niemal jednoczesnie. Uzywa tej samej,
 * juz przetestowanej, nie-destrukcyjnej sciezki co awaryjny failover
 * (descrambler_standby_promote(), ktora sama wybiera najszybszego
 * sposrod dostepnych kandydatow po sredniej - patrz zmiana tam wyzej).
 */
static void
descrambler_maybe_switch_to_faster
  ( service_t *t, th_descrambler_runtime_t *dr, th_descrambler_t *td )
{
  th_descrambler_t *active;
  int64_t now;

  if (!ecm_race_enabled())
    return;
  active = t->s_descrambler;
  if (active == NULL || active == td || td->td_keystate == DS_RESOLVED)
    return;
  if (!td->td_standby_valid)
    return;
  if (td->td_ecm_count == 0 || active->td_ecm_count == 0)
    return; /* jeszcze zadnego pomiaru po ktorejs ze stron - nie ma czego porownywac */

  now = mclk();
  if (dr->dr_last_speed_switch &&
      dr->dr_last_speed_switch + sec2mono(ECM_RACE_SWITCH_COOLDOWN) > now)
    return;

  if (active->td_ecm_time_last < ECM_RACE_SWITCH_MIN_MARGIN_MS)
    return; /* aktywny juz jest szybki - nie ma sensu ryzykowac przelaczenia */
  /*
   * bugfix: td_ecm_time_last jest uint32_t - odejmowanie bez tego
   * warunku, gdy td jest w rzeczywistosci WOLNIEJSZY (last >= active),
   * podkreciloby sie (unsigned underflow) do ogromnej liczby i
   * BLEDNIE przeszlo test ponizej jako "wystarczajaco szybszy".
   */
  if (td->td_ecm_time_last >= active->td_ecm_time_last)
    return; /* nie szybszy w ogole tym razem */
  if (active->td_ecm_time_last - td->td_ecm_time_last < ECM_RACE_SWITCH_MIN_MARGIN_MS)
    return; /* niewystarczajaco szybszy - moze byc zwykly szum */

  dr->dr_last_speed_switch = now;
  tvhinfo(LS_DESCRAMBLER,
          "%s: switching to faster reader for service \"%s\" "
          "(last %ums vs current active last %ums)",
          td->td_nicename, t->s_nicename,
          td->td_ecm_time_last, active->td_ecm_time_last);
  descrambler_standby_promote(t, dr);
}

void
descrambler_notify( th_descrambler_t *td,
                    uint16_t caid, uint32_t provid,
                    const char *cardsystem, uint16_t pid, uint32_t ecmtime,
                    uint16_t hops, const char *reader, const char *from,
                    const char *protocol )
{
  mpegts_service_t *t = (mpegts_service_t *)td->td_service;
  descramble_info_t *di;

  tvhdebug(LS_DESCRAMBLER, "info - service='%s' caid=%04X(%s) "
                                   "provid=%06X ecmtime=%d hops=%d "
                                   "reader='%s' from='%s' protocol='%s'%s",
         t->s_dvb_svcname, caid, cardsystem, provid,
         ecmtime, hops, reader, from, protocol,
         t->s_descrambler != td ? " (inactive)" : "");

  /* nowosc (#3): licz metryki dla KAZDEGO czytnika, tez nieaktywnego -
     o to wlasnie chodzi (porownanie serwerow). */
  tvh_mutex_lock(&t->s_stream_mutex);
  /*
   * bugfix: td_caid ustawiany byl tylko raz, przy starcie readera
   * (zgadywany dla capmt/capmt2 jako "pierwszy CAID z PMT" - patrz
   * komentarz w CaReadersView.vue - a dla cccam/cwc jako CAID karty
   * wybranej przy cc_service_start()). Tutaj mamy PRAWDZIWY, aktualnie
   * uzywany CAID z kazdego udanego ECM (dla capmt2 wprost z komunikatu
   * OSCam ECM_INFO) - nadpisujemy nim, wiec Status -> CA Readers i
   * Subscriptions pokazuja teraz ten sam, prawdziwy CAID zamiast
   * rozjezdzajacej sie zgadywanki.
   */
  td->td_caid = caid;
  descrambler_reader_stat_ecm(td, ecmtime);
  if (((td->td_ecm_count + td->td_ecm_nok) & 15) == 0)
    descrambler_reader_stats_dump((service_t *)t, "periodic");
  tvh_mutex_unlock(&t->s_stream_mutex);

  /*
   * nowosc: sprawdz TERAZ, zanim ewentualnie wyjdziemy jako "inactive"
   * ponizej - to jest dokladnie ten scenariusz (odpowiedz od cieplego
   * czytnika standby, nie od aktywnego).
   */
  if (((service_t *)t)->s_descramble)
    descrambler_maybe_switch_to_faster((service_t *)t, ((service_t *)t)->s_descramble, td);

  if (t->s_descrambler != td)
    return;

  di = calloc(1, sizeof(*di));

  di->pid     = pid;
  di->caid    = caid;
  di->provid  = provid;
  di->ecmtime = ecmtime;
  di->hops    = hops;
  strlcpy(di->cardsystem, cardsystem, sizeof(di->cardsystem));
  strlcpy(di->reader, reader, sizeof(di->reader));
  strlcpy(di->from, from, sizeof(di->from));
  strlcpy(di->protocol, protocol, sizeof(di->protocol));

  tvh_mutex_lock(&t->s_stream_mutex);
  descrambler_notify_deliver(t, di);
  tvh_mutex_unlock(&t->s_stream_mutex);
}

int
descrambler_resolved( service_t *t, th_descrambler_t *ignore )
{
  th_descrambler_t *td;

  LIST_FOREACH(td, &t->s_descramblers, td_service_link)
    if (td != ignore && td->td_keystate == DS_RESOLVED)
      return 1;
  return 0;
}

int
descrambler_multi_pid ( th_descrambler_t *td )
{
  service_t *t = td->td_service;
  th_descrambler_runtime_t *dr;

  if (t == NULL || (dr = t->s_descramble) == NULL)
    return 0;
  return dr->dr_key_multipid;
}

static struct strtab keystatetab[] = {
  { "INIT",       DS_INIT },
  { "READY",      DS_READY },
  { "RESOLVED",   DS_RESOLVED },
  { "FORBIDDEN",  DS_FORBIDDEN },
  { "FATAL",      DS_FATAL },
  { "IDLE",       DS_IDLE },
};

const char *
descrambler_keystate2str( th_descrambler_keystate_t keystate )
{
  return val2str(keystate, keystatetab) ?: "INVALID";
}

void
descrambler_change_keystate( th_descrambler_t *td, th_descrambler_keystate_t keystate, int lock )
{
  service_t *t = td->td_service;
  th_descrambler_runtime_t *dr;
  int count = 0, fatal = 0, failed = 0, resolved = 0;

  if (td->td_keystate == keystate)
    return;

  tvhtrace(LS_DESCRAMBLER, "%s: key state changed from %s to %s for \"%s\"",
                           td->td_nicename,
                           descrambler_keystate2str(td->td_keystate),
                           descrambler_keystate2str(keystate),
                           t->s_nicename);
  if (keystate == DS_FORBIDDEN)
    td->td_ecm_nok++;   /* nowosc (#3): metryka "access denied" per czytnik */
  td->td_keystate = keystate;
  if (t == NULL || (dr = t->s_descramble) == NULL)
    return;

  if (lock)
    tvh_mutex_lock(&t->s_stream_mutex);
  count = failed = resolved = 0;
  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    count++;
    switch (td->td_keystate) {
    case DS_FATAL:     fatal++;    break;
    case DS_FORBIDDEN: failed++;   break;
    case DS_RESOLVED : resolved++; break;
    default: break;
    }
  }
  dr->dr_ca_count = count;
  dr->dr_ca_resolved = resolved;
  dr->dr_ca_failed = failed;
  dr->dr_ca_fatal = fatal;
  tvhtrace(LS_DESCRAMBLER, "service \"%s\": %d descramblers (%d ok %d failed %d fatal)",
                           t->s_nicename, count, resolved, failed, fatal);
  if (lock)
    tvh_mutex_unlock(&t->s_stream_mutex);
}

static struct strtab keytypetab[] = {
  { "NONE",       DESCRAMBLER_NONE },
  { "CSA",        DESCRAMBLER_CSA_CBC },
  { "DES",        DESCRAMBLER_DES_NCB },
  { "AES EBC",    DESCRAMBLER_AES_ECB },
  { "AES128 EBC", DESCRAMBLER_AES128_ECB },
};

const char *
descrambler_keytype2str( th_descrambler_keystate_t keytype )
{
  return val2str(keytype, keytypetab) ?: "INVALID";
}

void
descrambler_keys ( th_descrambler_t *td, int type, uint16_t pid,
                   const uint8_t *even, const uint8_t *odd )
{
  static uint8_t empty[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  service_t *t = td->td_service;
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  th_descrambler_t *td2;
  char pidname[16];
  const char *ktype;
  uint16_t pid2;
  int j, changed = 0, insert = 0;

  if (t == NULL || (dr = t->s_descramble) == NULL) {
    descrambler_change_keystate(td, DS_FORBIDDEN, 1);
    return;
  }

  tvh_mutex_lock(&t->s_stream_mutex);

  if (pid == 0 && dr->dr_key_multipid) {
    for (j = 0; j < DESCRAMBLER_MAX_KEYS; j++) {
      tk = &dr->dr_keys[j];
      pid2 = tk->key_pid;
      if (pid2) {
        tvh_mutex_unlock(&t->s_stream_mutex);
        descrambler_keys(td, type, pid2, even, odd);
        tvh_mutex_lock(&t->s_stream_mutex);
      }
    }
    goto end;
  }

  if (!dr->dr_key_multipid)
    pid = 0;

  for (j = 0; j < DESCRAMBLER_MAX_KEYS; j++) {
    tk = &dr->dr_keys[j];
    pid2 = tk->key_pid;
    if (pid2 == 0 || pid2 == pid) break;
  }

  if (j >= DESCRAMBLER_MAX_KEYS) {
    tvherror(LS_DESCRAMBLER, "too many keys");
    goto end;
  }

  if (pid == 0)
    pidname[0] = '\0';
  else
    snprintf(pidname, sizeof(pidname), "[%d]", pid);
  ktype = descrambler_keytype2str(type);

  if (tvhcsa_set_type(&tk->key_csa, (mpegts_service_t *)t, type) < 0) {
    if (tk->key_type_overwritten)
      goto end;
    if (type == DESCRAMBLER_CSA_CBC && tk->key_csa.csa_type == DESCRAMBLER_DES_NCB) {
      tvhwarn(LS_DESCRAMBLER,
              "Keep key%s type %s (requested %s) for service \"%s\", check your caclient",
              pidname, descrambler_keytype2str(tk->key_csa.csa_type), ktype,
              ((mpegts_service_t *)t)->s_dvb_svcname);
      goto cont;
    }
    tk->key_type_overwritten = 1;
    tvhwarn(LS_DESCRAMBLER,
            "Overwrite key%s type from %s to %s for service \"%s\"",
            pidname, descrambler_keytype2str(tk->key_csa.csa_type),
            ktype, ((mpegts_service_t *)t)->s_dvb_svcname);
    tvhcsa_destroy(&tk->key_csa);
    tvhcsa_init(&tk->key_csa);
    if (tvhcsa_set_type(&tk->key_csa, (mpegts_service_t *)t, type) < 0)
      goto end;
    tk->key_valid = 0;
  }

cont:
  LIST_FOREACH(td2, &t->s_descramblers, td_service_link)
    if (td2 != td && td2->td_keystate == DS_RESOLVED) {
      tvhdebug(LS_DESCRAMBLER,
               "Already has a key[%d] from %s for service \"%s\", "
               "ignoring key from \"%s\"%s",
               tk->key_pid, td2->td_nicename,
               ((mpegts_service_t *)td2->td_service)->s_dvb_svcname,
               td->td_nicename,
               dr->dr_key_const ? " (const)" : "");
      /*
       * nowosc: zamiast calkowicie porzucac ten klucz, zachowaj go jako
       * "zapasowy" w samym td. Patrz ecm_reset() - jesli aktywny klient
       * sie spozni, ten cache pozwala przelaczyc sie natychmiast, bez
       * czekania na kolejna pelna wymiane ECM.
       */
      td->td_standby_pid   = pid;
      td->td_standby_type  = type;
      td->td_standby_valid = 0;
      if (even && memcmp(empty, even, DESCRAMBLER_KEY_SIZE(type))) {
        memcpy(td->td_standby_even, even, DESCRAMBLER_KEY_SIZE(type));
        td->td_standby_valid |= 1;
      }
      if (odd && memcmp(empty, odd, DESCRAMBLER_KEY_SIZE(type))) {
        memcpy(td->td_standby_odd, odd, DESCRAMBLER_KEY_SIZE(type));
        td->td_standby_valid |= 2;
      }
      td->td_standby_time = mclk();
      /*
       * nowosc (#1): w trybie wyscigu ECM NIE usypiamy tego czytnika -
       * zostaje "cieply" (DS_READY), dalej podaza za zmianami ECM i przy
       * kazdym kluczu odswieza powyzszy cache standby. Dzieki temu
       * failover ma zawsze swiezy zapas z niezaleznego serwera. W trybie
       * domyslnym czytnik idzie w DS_IDLE (td_ecm_idle) jak dotychczas.
       */
      if (ecm_race_enabled()) {
        if (td->td_keystate != DS_READY) {
          tvhinfo(LS_DESCRAMBLER,
                  "ECM race: keeping reader %s warm for service \"%s\" "
                  "(standby refreshed)", td->td_nicename, t->s_nicename);
          descrambler_change_keystate(td, DS_READY, 0);
        }
      } else {
        descrambler_change_keystate(td, DS_IDLE, 0);
        descrambler_reader_stats_dump(t, "parked");
        if (td->td_ecm_idle) {
          tvh_mutex_unlock(&t->s_stream_mutex);
          td->td_ecm_idle(td);
          tvh_mutex_lock(&t->s_stream_mutex);
        }
      }
      goto end;
    }

  if (even && memcmp(empty, even, tk->key_csa.csa_keylen)) {
    memcpy(tk->key_data[0], even, tk->key_csa.csa_keylen);
    tk->key_pid = pid;
    changed |= 1;
    if (tk->key_timestamp[0] == 0 ||
        descrambler_data_key_check(dr, 0x80, dr->dr_queue_total) >= 0)
      insert |= 1;
    tk->key_timestamp[0] = mclk();
    if (dr->dr_ecm_start[0] < dr->dr_ecm_start[1]) {
      dr->dr_ecm_start[0] = dr->dr_ecm_start[1];
      tvhdebug(LS_DESCRAMBLER,
               "Both keys received, marking ECM start for even key%s for service \"%s\"",
               pidname, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
  } else {
    even = empty;
  }
  if (odd && memcmp(empty, odd, tk->key_csa.csa_keylen)) {
    memcpy(tk->key_data[1], odd, tk->key_csa.csa_keylen);
    tk->key_pid = pid;
    changed |= 2;
    if (tk->key_timestamp[1] == 0 ||
        descrambler_data_key_check(dr, 0xc0, dr->dr_queue_total) >= 0)
      insert |= 2;
    tk->key_timestamp[1] = mclk();
    if (dr->dr_ecm_start[1] < dr->dr_ecm_start[0]) {
      dr->dr_ecm_start[1] = dr->dr_ecm_start[0];
      tvhdebug(LS_DESCRAMBLER,
               "Both keys received, marking ECM start for odd key%s for service \"%s\"",
               pidname, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
  } else {
    odd = empty;
  }

  if (changed) {
    /*
     * nowosc (nowa #1 - adaptacyjny bufor descramblera): dr_queue_total w
     * tym momencie to dokladnie tyle danych, ile naplynelo zanim TEN klucz
     * dotarl - naturalny "znak wodny" na realne zapotrzebowanie na bufor
     * (patrz uzycie w galezi przepelnienia w descrambler_descramble()).
     * EWMA + margines 1.5x. Dolny prog to nadal config.descrambler_buffer
     * ustawiony recznie - to uczenie WYLACZNIE podnosi bufor w gore, gdy
     * kanal go realnie potrzebuje (np. wolny serwer ECM), nigdy go nie
     * zmniejsza ponizej skonfigurowanej wartosci.
     */
    if (config.descrambler_buffer_adaptive) {
      uint32_t watermark = ((dr->dr_queue_total / 188) * 3) / 2;
      dr->dr_adaptive_buflen = dr->dr_adaptive_buflen ?
        (dr->dr_adaptive_buflen * 3 + watermark) / 4 : watermark;
    }
    descrambler_data_add_key(dr, tk, changed, insert);
    if (td->td_keystate != DS_RESOLVED)
      tvhdebug(LS_DESCRAMBLER,
               "Obtained %s keys%s from %s for service \"%s\"%s",
               ktype, pidname, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname,
               dr->dr_key_const ? " (const)" : "");
    if (tk->key_csa.csa_keylen == 8) {
      tvhtrace(LS_DESCRAMBLER, "Obtained %s keys%s "
               "%02X%02X%02X%02X%02X%02X%02X%02X:%02X%02X%02X%02X%02X%02X%02X%02X"
               " pid %04X from %s for service \"%s\"",
               ktype, pidname,
               even[0], even[1], even[2], even[3], even[4], even[5], even[6], even[7],
               odd[0], odd[1], odd[2], odd[3], odd[4], odd[5], odd[6], odd[7],
               pid, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname);
    } else if (tk->key_csa.csa_keylen == 16) {
      tvhtrace(LS_DESCRAMBLER, "Obtained %s keys%s "
               "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X:"
               "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"
               " pid %04X from %s for service \"%s\"",
               ktype, pidname,
               even[0], even[1], even[2], even[3], even[4], even[5], even[6], even[7],
               even[8], even[9], even[10], even[11], even[12], even[13], even[14], even[15],
               odd[0], odd[1], odd[2], odd[3], odd[4], odd[5], odd[6], odd[7],
               odd[8], odd[9], odd[10], odd[11], odd[12], odd[13], odd[14], odd[15],
               pid, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname);
    } else {
      tvhtrace(LS_DESCRAMBLER, "Unknown keys%s pid %04X from %s for for service \"%s\"",
               pidname, pid, td->td_nicename, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
    dr->dr_ecm_last_key_time = mclk();
    descrambler_change_keystate(td, DS_RESOLVED, 0);
    td->td_service->s_descrambler = td;
  } else {
    tvhdebug(LS_DESCRAMBLER,
             "Empty %s keys%s received from %s for service \"%s\"%s",
             ktype, pidname, td->td_nicename,
             ((mpegts_service_t *)t)->s_dvb_svcname,
             dr->dr_key_const ? " (const)" : "");
  }

end:
  tvh_mutex_unlock(&t->s_stream_mutex);
}

void
descrambler_flush_table_data( service_t *t )
{
  mpegts_service_t *ms = (mpegts_service_t *)t;
  mpegts_mux_t *mux = ms->s_dvb_mux;
  descrambler_table_t *dt;
  descrambler_section_t *ds;

  if (mux == NULL)
    return;
  tvhtrace(LS_DESCRAMBLER, "flush table data for service \"%s\"", ms->s_dvb_svcname);
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table == NULL || dt->table->mt_service != ms)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link)
      descrambler_destroy_all_ecmsecs(ds);
  }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
}

static inline void
key_update( th_descrambler_runtime_t *dr, service_t *t, th_descrambler_key_t *tk, uint8_t key, int64_t timestamp )
{
  int64_t new_margin;

  /* set the even (0) or odd (0x40) key index */
  tk->key_index = key & 0x40;
  if (tk->key_start) {
    /* don't change key interval for fast rolling keys */
    if (tk->key_initial_interval > 2000000) {
      tk->key_interval = tk->key_start + sec2mono(50) < timestamp ?
                         tk->key_initial_interval : MAX(5000000, timestamp - tk->key_start);
      tvhtrace(LS_DESCRAMBLER, "update key[%d] interval for \"%s\" to %ldms", tk->key_pid, t->s_nicename, (long)(tk->key_interval / 1000));
      /*
       * nowosc (nowa #3 - adaptacyjny margines ECM / "pre-roll"): TVH nie
       * moze poprosic nadawcy o wczesniejsze ECM - to nadawca decyduje,
       * kiedy wysyla sekcje ECM dla kolejnego okresu kryptograficznego.
       * To, co MOZEMY zrobic, to trzymac tolerancje czasowa uzywana do
       * wykrywania "spoznionego" klucza (dr_ecm_key_margin, patrz
       * key_changed()/key_late()) dopasowana do REALNEGO rytmu tego
       * kanalu, a nie do statycznej wartosci "interval/5" z pliku
       * podpowiedzi zamrozonej raz przy starcie uslugi. tk->key_interval
       * powyzej juz sledzi rzeczywisty odstep miedzy rotacjami - margines
       * plynnie (EWMA) za nim podaza. Ograniczony do [1s, interval/3],
       * zeby pojedynczy szumowy pomiar nie rozregulowal wykrywania
       * spoznionego klucza.
       */
      new_margin = MINMAX(tk->key_interval / 5, sec2mono(1), tk->key_interval / 3);
      dr->dr_ecm_key_margin = dr->dr_ecm_key_margin ?
        (dr->dr_ecm_key_margin * 3 + new_margin) / 4 : new_margin;
    }
    tk->key_start = timestamp;
  } else {
    /* We don't know the exact start key switch time */
    tk->key_start = timestamp - sec2mono(60);
  }
}

static inline int
key_changed ( th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, uint8_t ki, int64_t timestamp )
{
  return tk->key_index != (ki & 0x40) &&
         tk->key_start + dr->dr_ecm_key_margin < timestamp;
}

static inline int
key_valid ( th_descrambler_key_t *tk, uint8_t ki )
{
  /* 0x40 (for even) or 0x80 (for odd) */
  uint8_t mask = ((ki & 0x40) + 0x40);
  return tk && (tk->key_valid & mask);
}

static inline int
key_late( th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, uint8_t ki, int64_t timestamp )
{
  uint8_t kidx = (ki & 0x40) >> 6;
  /* constcw - do not handle keys */
  if (dr->dr_key_const)
    return 0;
  /* required key is older than previous? */
  if (tk->key_timestamp[kidx] < tk->key_timestamp[kidx^1]) {
    /* but don't take in account the keys modified just now */
    if (tk->key_timestamp[kidx^1] + ms2mono(350) < timestamp)
      goto late;
  }
  /* ECM was sent, but no new key was received */
  if (dr->dr_ecm_last_key_time + dr->dr_ecm_key_margin < tk->key_start &&
      (!dr->dr_quick_ecm || dr->dr_ecm_start[kidx] + ms2mono(10) < tk->key_start)) {
late:
    tk->key_valid &= ~((ki & 0x40) + 0x40);
    return 1;
  }
  return 0;
}

static inline int
key_started( th_descrambler_runtime_t *dr, uint8_t ki )
{
  uint8_t kidx = (ki & 0x40) >> 6;
  return mclk() - dr->dr_ecm_start[kidx] < dr->dr_ecm_key_margin * 2;
}

static void
old_key_flush ( th_descrambler_runtime_t *dr, service_t *t )
{
  th_descrambler_key_t *tk = dr->dr_key_last;

  if (tk) {
    debug2("%p: key[%d] flush1", dr, tk->key_pid);
    tk->key_csa.csa_flush(&tk->key_csa, (mpegts_service_t *)t);
    dr->dr_key_last = NULL;
  }
}

static void
key_flush( th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, uint8_t changed, service_t *t )
{
  if (!changed)
    return;
  debug2("%p: key[%d] flush2", dr, tk->key_pid);
  tk->key_csa.csa_flush(&tk->key_csa, (mpegts_service_t *)t);
  /* update the keys */
  if (changed & 1) {
    debug2("%p: even key[%d] set for decoder", dr, tk->key_pid);
    tvhcsa_set_key_even(&tk->key_csa, tk->key_data[0]);
    tk->key_valid |= 0x40;
  }
  if (changed & 2) {
    debug2("%p: odd key[%d] set for decoder", dr, tk->key_pid);
    tvhcsa_set_key_odd(&tk->key_csa, tk->key_data[1]);
    tk->key_valid |= 0x80;
  }
}

static th_descrambler_key_t *
key_find_struct( th_descrambler_runtime_t *dr,
                 const uint8_t *tsb,
                 service_t *t )
{
  th_descrambler_key_t *tk;
  int i, pid = extractpid(tsb);
  if (dr->dr_key_last && dr->dr_key_last->key_pid == pid)
    return dr->dr_key_last;
  for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
    tk = &dr->dr_keys[i];
    if (tk->key_pid == 0)
      break;
    if (tk->key_pid == pid) {
      old_key_flush(dr, t);
      return tk;
    }
  }
  return NULL;
}

/*
 * nowosc: wydzielone z ecm_reset() do osobnej, publicznej funkcji - ta sama
 * logika promocji cache'owanego klucza zapasowego jest teraz potrzebna
 * takze poza "klucz sie spoznil" (key_late() w descrambler_descramble()).
 * Konkretnie: gdy AKTUALNIE AKTYWNY (DS_RESOLVED) klient CA sam dostaje
 * "access denied" (NOK) dla wlasnego, kolejnego ECM, cclient.c demontuje
 * go bezposrednio na DS_FORBIDDEN - z pominieciem ecm_reset() w ogole.
 * Bez tego wywolania fast failover nigdy nie mial szansy zadzialac w tym
 * scenariuszu (patrz wywolanie w cc_ecm_reply() w cclient.c), mimo ze
 * inny reader mogl miec w tym momencie idealnie swiezy cache standby.
 */
int
descrambler_standby_promote( service_t *t, th_descrambler_runtime_t *dr )
{
  th_descrambler_t *td, *promote = NULL;
  int64_t now = mclk();
  uint8_t standby_even[16], standby_odd[16];
  uint8_t standby_valid = 0, standby_type = 0;
  uint16_t standby_pid = 0;
  uint32_t promote_avg = 0, avg;

  /*
   * bugfix: td_standby_* jest zapisywane pod t->s_stream_mutex w
   * descrambler_keys(), wiec czytamy/kasujemy je tutaj pod tym samym
   * lockiem, zeby uniknac wyscigu (mozliwy odczyt rozdartego klucza,
   * gdy inny watek klienta CA akurat zapisuje standby w tym samym
   * momencie). Kopiujemy dane do lokalnych zmiennych i zwalniamy lock
   * PRZED wywolaniem td_ecm_reset()/descrambler_keys() nizej - te
   * moga same wewnetrznie chciec zablokowac ten sam mutex.
   */
  tvh_mutex_lock(&t->s_stream_mutex);
  /*
   * nowosc (diagnostyka): tymczasowy trace pokazujacy realny stan cache'u
   * standby w momencie awarii - do usuniecia po zdiagnozowaniu, dlaczego
   * fast failover sie nie uruchamia mimo pozornie wystarczajacego
   * standby_age.
   */
  tvhtrace(LS_DESCRAMBLER,
           "standby_promote: service \"%s\" standby_age=%ldms, scanning descramblers",
           t->s_nicename, (long)(dr->dr_ecm_standby_age / 1000));
  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    tvhtrace(LS_DESCRAMBLER,
             "standby_promote:   %s keystate=%s standby_valid=%d age=%ldms",
             td->td_nicename, descrambler_keystate2str(td->td_keystate),
             td->td_standby_valid,
             (long)(td->td_standby_valid ? (now - td->td_standby_time) / 1000 : -1));
    if (td->td_keystate == DS_RESOLVED)
      continue;
    if (!td->td_standby_valid)
      continue;
    if (td->td_standby_time + dr->dr_ecm_standby_age < now) {
      tvhtrace(LS_DESCRAMBLER, "standby_promote:   %s standby too old, discarding",
               td->td_nicename);
      td->td_standby_valid = 0; /* zbyt stary - odrzuc */
      continue;
    }
    /*
     * nowosc: gdy jest kilku eligible kandydatow (typowo dzieki ECM
     * race - patrz config.descrambler_ecm_race), wybierz tego o
     * najlepszej dotychczasowej sredniej odpowiedzi ECM zamiast
     * pierwszego napotkanego w kolejnosci listy - to byla przypadkowa
     * kolejnosc rejestracji klientow, nie miala nic wspolnego z tym,
     * ktory realnie odpowiada szybciej. Brak probek (td_ecm_count==0)
     * traktujemy jako "nieznane, gorsze niz cokolwiek zmierzone", zeby
     * dalej preferowac sprawdzonego kandydata nad calkowicie nowym.
     */
    avg = td->td_ecm_count ? (uint32_t)(td->td_ecm_time_sum / td->td_ecm_count) : UINT32_MAX;
    if (promote == NULL || avg < promote_avg) {
      promote = td;
      promote_avg = avg;
    }
  }
  if (promote) {
    standby_valid = promote->td_standby_valid;
    standby_type  = promote->td_standby_type;
    standby_pid   = promote->td_standby_pid;
    if (standby_valid & 1)
      memcpy(standby_even, promote->td_standby_even, sizeof(standby_even));
    if (standby_valid & 2)
      memcpy(standby_odd, promote->td_standby_odd, sizeof(standby_odd));
    promote->td_standby_valid = 0;
    promote->td_failover_count++;  /* nowosc: widoczne w Status -> CA Readers */
  }
  tvh_mutex_unlock(&t->s_stream_mutex);

  if (!promote)
    return 0;

  tvhinfo(LS_DESCRAMBLER,
          "%s: fast failover - using cached standby key for service \"%s\" "
          "instead of full ECM reset", promote->td_nicename, t->s_nicename);
  LIST_FOREACH(td, &t->s_descramblers, td_service_link)
    if (td != promote && td->td_keystate == DS_RESOLVED)
      td->td_ecm_reset(td);
  descrambler_keys(promote, standby_type, standby_pid,
                    (standby_valid & 1) ? standby_even : NULL,
                    (standby_valid & 2) ? standby_odd  : NULL);
  return 1;
}

static int
ecm_reset( service_t *t, th_descrambler_runtime_t *dr )
{
  th_descrambler_t *td;
  th_descrambler_key_t *tk;
  int ret = 0, i;

  if (descrambler_standby_promote(t, dr))
    return 1;

  /* reset the reader ECM state */
  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    if (!td->td_ecm_reset(td)) {
      for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
        tk = &dr->dr_keys[i];
        tk->key_valid = 0;
        if (tk->key_pid == 0)
          break;
      }
      ret = 1;
    }
  }
  return ret;
}

int
descrambler_pass ( service_t *t,
                   elementary_stream_t *st,
                   const uint8_t *tsb,
                   int len )
{
  if ((tsb[3] & 0x80) == 0)
    ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
  return 1;
}

int
descrambler_descramble ( service_t *t,
                         elementary_stream_t *st,
                         const uint8_t *tsb,
                         int len )
{
  th_descrambler_runtime_t *dr = t->s_descramble;
  th_descrambler_key_t *tk;
  th_descrambler_data_t *dd, *dd_next;
  int len2, len3, r, flush_data, update_tk;
  uint32_t dbuflen;
  const uint8_t *tsb2;
  int64_t now;
  uint_fast8_t ki;
  sbuf_t *sb;

  lock_assert(&t->s_stream_mutex);

  if (dr == NULL) {
    if ((tsb[3] & 0x80) == 0) {
      ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
      return 1;
    }
    return -1;
  }

  if (dr->dr_descramble)
    return dr->dr_descramble(t, st, tsb, len);

  if (!dr->dr_key_multipid) {
    tk = &dr->dr_keys[0];
  } else {
    tk = (tsb[3] & 0x80) != 0 ? key_find_struct(dr, tsb, t) : dr->dr_key_last;
  }  

  if (dr->dr_queue_total == 0 && (tsb[3] & 0x80) == 0) {
    if (tk && tk->key_csa.csa_type != DESCRAMBLER_NONE) {
      debug2("%p: descramble0 %d, %s[%d]", dr, len, keystr(tsb), extractpid(tsb));
      tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb, len);
      dr->dr_key_last = tk;
    } else {
      old_key_flush(dr, t);
      debug2("%p: direct0 %d, %s[%d]", dr, len, keystr(tsb), extractpid(tsb));
      ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
    }
    return 1;
  }

  update_tk = 0;
  flush_data = 0;
  if (dr->dr_ca_resolved > 0) {

    /* process the queued TS packets or key updates */
    for (dd = TAILQ_FIRST(&dr->dr_queue); dd; dd = dd_next) {
      dd_next = TAILQ_NEXT(dd, dd_link);
      sb = &dd->dd_sbuf;
      tsb2 = sb->sb_data;
      len2 = sb->sb_ptr;
      if (dd->dd_key) {
        key_flush(dr, dd->dd_key, dd->dd_key_changed, t);
        dd->dd_key = NULL;
      }
      if (len2 == 0)
        goto dd_destroy;
      if ((tsb2[3] & 0x80) == 0) {
        if (tk == NULL) {
          tk = dr->dr_key_last;
          update_tk = 1;
        }
        if (tk) {
          debug2("%p: descramble1 %d, %s[%d]", dr, len2, keystr(tsb2), extractpid(tsb2));
          tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb2, len2);
          dr->dr_key_last = tk;
        } else {
          debug2("%p: direct1 %d, %s[%d]", dr, len2, keystr(tsb2), extractpid(tsb2));
          ts_recv_packet2((mpegts_service_t *)t, tsb2, len2);
        }
        goto dd_destroy;
      }
      if (dr->dr_key_multipid) {
        update_tk = 1;
        tk = key_find_struct(dr, tsb2, t);
        if (tk == NULL) {
          if (t->s_start_time + 3000000 < mclk() &&
              tvhlog_limit(&dr->dr_loglimit_key, 10))
            tvhwarn(LS_DESCRAMBLER, "%s stream key[%d] is not available",
                    ((mpegts_service_t *)t)->s_dvb_svcname, extractpid(tsb2));
          goto next;
        }
      }
      now = mclk();
#ifdef DEBUG2
      {
      int64_t t1, t2;
      t1 = dd->dd_timestamp;
      t2 = tk->key_interval - tk->key_interval / 5;
      debug2("%p: timestamp %ld thres %ld now %ld (interval %ldms) %s[%d]", dr, t1, now - t2, (now - t1) / 1000, t2 / 1000, keystr(tsb2), extractpid(tsb2));
      }
#endif
      if (dd->dd_timestamp < now - (tk->key_interval - tk->key_interval / 5)) {
        debug2("%p: ^^^ destroy\n", dr);
        descrambler_data_destroy(dr, dd, 1);
        continue;
      }
      for (; len2 > 0; tsb2 += len3, len2 -= len3) {
        ki = tsb2[3];
        if ((ki & 0x80) != 0x00) {
          if (key_valid(tk, ki) == 0)
            goto queue;
          if (key_changed(dr, tk, ki, dd->dd_timestamp)) {
            r = descrambler_data_analyze(dr, dd, ki);
            if (r == 0) {
              /* wait for more data to decide */
              descrambler_data_cut(dr, tsb2 - sb->sb_data);
              descrambler_data_append(dr, tsb, len);
              goto end;
            } else if (r == 2)
              goto doit;
            tvhtrace(LS_DESCRAMBLER, "stream key[%d] changed to %s for service \"%s\"",
                                    tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                    ((mpegts_service_t *)t)->s_dvb_svcname);
            if (key_late(dr, tk, ki, dd->dd_timestamp)) {
              descrambler_notify_nokey(dr);
              tvh_mutex_unlock(&t->s_stream_mutex);
              r = ecm_reset(t, dr);
              tvh_mutex_lock(&t->s_stream_mutex);
              if (r) {
                descrambler_data_cut(dr, tsb2 - sb->sb_data);
                flush_data = 1;
                goto queue;
              }
            }
            key_update(dr, t, tk, ki, dd->dd_timestamp);
          }
        }
doit:
        len3 = mpegts_word_count(tsb2, len2, 0xFF0000C0);
        debug2("%p: descramble2 %d, %s[%d]", dr, len3, keystr(tsb2), extractpid(tsb2));
        tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb2, len3);
        dr->dr_key_last = tk;
      }
      if (len2 == 0) {
        service_reset_streaming_status_flags(t, TSS_NO_ACCESS);
        dr->dr_ok_time = mclk();   /* nowosc (#4): wyjscie zyje */
      }
dd_destroy:
      descrambler_data_destroy(dr, dd, 0);
    }

    if (update_tk) {
      tk = key_find_struct(dr, tsb, t);
      if (tk == NULL) {
        if ((tsb[3] & 0x80) == 0) {
          ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
          return 1;
        }
        goto next;
      }
    }

    /* check for key change */
    ki = tsb[3];
    if ((ki & 0x80) != 0x00) {
      if (key_valid(tk, ki) == 0) {
        if (!key_started(dr, ki) && tvhlog_limit(&dr->dr_loglimit_key, 10))
          tvhwarn(LS_DESCRAMBLER, "%s %s stream key[%d] is not valid",
                   ((mpegts_service_t *)t)->s_dvb_svcname,
                   (ki & 0x40) ? "odd" : "even", extractpid(tsb));
        goto next;
      }
      if (key_changed(dr, tk, ki, mclk())) {
        /* postpone the key change */
        descrambler_data_append(dr, tsb, len);
        goto end;
      }
    }
    dr->dr_skip = 1;
    debug2("%p: descramble3 %d, %s[%d]", dr, len, keystr(tsb), extractpid(tsb));
    tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb, len);
    dr->dr_key_last = tk;
    service_reset_streaming_status_flags(t, TSS_NO_ACCESS);
    dr->dr_ok_time = mclk();   /* nowosc (#4): wyjscie zyje */
    return 1;
  }
next:
  if (!dr->dr_skip) {
    if (!dr->dr_force_skip)
      dr->dr_force_skip = mclk() + sec2mono(30);
    else if (dr->dr_force_skip < mclk())
      dr->dr_skip = 1;
  }
  if (dr->dr_ecm_start[0] || dr->dr_ecm_start[1]) { /* ECM sent */
    ki = tsb[3];
    if ((ki & 0x80) != 0x00) {
      if (dr->dr_key_multipid) {
        tk = key_find_struct(dr, tsb, t);
        if (tk == NULL) goto queue;
      } else {
        tk = &dr->dr_keys[0];
      }
      if (tk->key_start == 0) {
        /* do not use the first TS packet to decide - it may be wrong */
        while (dr->dr_queue_total > dr->dr_initial_paritycheck) {
          if (descrambler_data_key_check(dr, ki & 0xc0, dr->dr_initial_paritycheck) == 0) {
            tvhtrace(LS_DESCRAMBLER, "initial stream key[%d] set to %s for service \"%s\"",
                                    tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                    ((mpegts_service_t *)t)->s_dvb_svcname);
            key_update(dr, t, tk, ki, mclk());
            break;
          } else {
            descrambler_data_cut(dr, 188);
          }
        }
      } else if (key_changed(dr, tk, ki, mclk())) {
        tvhtrace(LS_DESCRAMBLER, "stream key[%d] changed to %s for service \"%s\"",
                                tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                ((mpegts_service_t *)t)->s_dvb_svcname);
        key_update(dr, t, tk, ki, mclk());
      }
    }
queue:
    if (dr->dr_ca_count != dr->dr_ca_failed) {
      /*
       * Fill a temporary buffer until the keys are known to make
       * streaming faster.
       */
      dbuflen = MAX(300, config.descrambler_buffer);
      if (config.descrambler_buffer_adaptive && dr->dr_adaptive_buflen > dbuflen)
        /* nowosc (nowa #1): pozwol wyuczonemu bufor rosnac ponad prog
           skonfigurowany recznie, ale ograniczony do 4x - zeby jeden
           wyjatkowo wolny ECM nie napompowal pamieci bez limitu */
        dbuflen = MIN(dr->dr_adaptive_buflen, dbuflen * 4);
      if (dr->dr_queue_total >= dbuflen * 188) {
        descrambler_data_cut(dr, MAX((dbuflen / 10) * 188, len));
        if (dr->dr_last_err + sec2mono(10) < mclk()) {
          dr->dr_last_err = mclk();
          tvherror(LS_DESCRAMBLER, "cannot decode packets for service \"%s\"",
                   ((mpegts_service_t *)t)->s_dvb_svcname);
        } else {
          tvhtrace(LS_DESCRAMBLER, "cannot decode packets for service \"%s\"",
                   ((mpegts_service_t *)t)->s_dvb_svcname);
        }
        /*
         * nowosc (#4 - dozorca wyjscia): wejscie sypie danymi (bufor
         * pelny), ale wyjscie stoi. Jesli juz kiedys dekodowalismy
         * (dr_ok_time != 0) i cisza trwa dluzej niz 2x interval (min 3s),
         * sprobuj NIE-DESTRUKCYJNEJ promocji swiezego standby z innego
         * czytnika - zamiast biernie czekac az aktywny czytnik sam
         * dokonczy nowy cykl ECM. Rate limit 5s. Gdy nie ma czego
         * promowac, swiadomie nic wiecej nie robimy (nie ruszamy waznych
         * kluczy - to bylo zrodlem regresji przy pierwszej wersji).
         */
        now = mclk();
        if (dr->dr_ok_time &&
            dr->dr_ok_time + MAX(sec2mono(3), dr->dr_keys[0].key_interval * 2) < now &&
            dr->dr_watchdog_last + sec2mono(5) < now) {
          dr->dr_watchdog_last = now;
          tvhwarn(LS_DESCRAMBLER,
                  "output stalled ~%"PRId64"ms for service \"%s\", trying standby failover",
                  mono2ms(now - dr->dr_ok_time),
                  ((mpegts_service_t *)t)->s_dvb_svcname);
          tvh_mutex_unlock(&t->s_stream_mutex);
          if (!descrambler_standby_promote(t, dr)) {
            /*
             * nowosc: nikt inny nie mial gotowego zapasu - poszturchaj
             * SAMEGO aktywnego czytnika, zeby natychmiast ponowil
             * biezace ECM, zamiast biernie czekac az samo dojdzie w
             * naturalnym cyklu. Wolamy td_ecm_reset() BEZPOSREDNIO na
             * tym jednym czytniku - NIGDY przez ogolny ecm_reset()
             * (ten, gdy klient zwraca 0 z td_ecm_reset - jak
             * capmt_ecm_reset()/cc_ecm_reset() - uniewaznia WSZYSTKIE
             * klucze w dr, co bylo zrodlem wczesniejszej regresji w
             * tej sesji: zdrowy, tylko chwilowo spozniony czytnik
             * tracil jeszcze wazny klucz w polowie cyklu). Bezposrednie
             * wywolanie resetuje WYLACZNIE wewnetrzny stan tego klienta
             * (np. cc_ecm_reset() czysci cache ostatnio wyslanej sekcji
             * ECM, wiec kolejne, nawet identyczne powtorzenie tej samej
             * sekcji w transporcie - DVB nadaje ja wielokrotnie w ciagu
             * okresu kryptograficznego - zostanie potraktowane jako
             * nowe i ponowione do serwera karty, zamiast czekac
             * bezczynnie az tresc ECM faktycznie sie zmieni).
             */
            th_descrambler_t *cur = t->s_descrambler;
            if (cur && cur->td_ecm_reset)
              cur->td_ecm_reset(cur);
          }
          tvh_mutex_lock(&t->s_stream_mutex);
        }
      }
      descrambler_data_append(dr, tsb, len);
      service_set_streaming_status_flags(t, TSS_NO_ACCESS);
    }
  } else {
    if (dr->dr_skip || dr->dr_ca_count == 0)
      ts_skip_packet2((mpegts_service_t *)t, tsb, len);
    service_set_streaming_status_flags(t, TSS_NO_ACCESS);
  }
  if (flush_data)
    descrambler_flush_table_data(t);
end:
  debug2("%p: end, %s", dr, keystr(tsb));
  if (dr->dr_ca_count > 0) {
    if (dr->dr_ca_count == dr->dr_ca_fatal)
      return 0;
    if (dr->dr_ca_count == dr->dr_ca_failed)
      return -1;
  }
  return dr->dr_ca_count;
}

static int
descrambler_table_callback
  (mpegts_table_t *mt, const uint8_t *ptr, int len, int tableid)
{
  descrambler_table_t *dt = mt->mt_opaque;
  descrambler_section_t *ds;
  descrambler_ecmsec_t *des;
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  LIST_HEAD(,descrambler_ecmsec) sections;
  int emm = (mt->mt_flags & MT_FAST) == 0;
  mpegts_service_t *t;
  int64_t clk, clk2, clk3;
  uint8_t ki;
  int i, j;
  caid_t *ca;
  elementary_stream_t *st;

  if (len < 6)
    return 0;
  clk = mclk();
  LIST_INIT(&sections);
  tvh_mutex_lock(&mt->mt_mux->mm_descrambler_lock);
  TAILQ_FOREACH(ds, &dt->sections, link) {
    if (!emm) {
      LIST_FOREACH(des, &ds->ecmsecs, link)
        if (des->number == ptr[4])
          break;
    } else {
      des = LIST_FIRST(&ds->ecmsecs);
    }
    if (des == NULL) {
      des = calloc(1, sizeof(*des));
      des->number = emm ? 0 : ptr[4];
      atomic_add(&des->refcnt, 1);
      LIST_INSERT_HEAD(&ds->ecmsecs, des, link);
    }
    if (des->last_data == NULL || len != des->last_data_len ||
        memcmp(des->last_data, ptr, len)) {
      free(des->last_data);
      des->last_data = malloc(len);
      if (des->last_data) {
        memcpy(des->last_data, ptr, len);
        des->last_data_len = len;
      } else {
        des->last_data_len = 0;
      }
      des->changed = 2;
    } else {
      des->changed = des->last_data != NULL ? 1 : 0;
    }
    des->callback = ds->callback;
    des->opaque = ds->opaque;
    atomic_add(&des->refcnt, 1);
    LIST_INSERT_HEAD(&sections, des, active_link);
  }
  tvh_mutex_unlock(&mt->mt_mux->mm_descrambler_lock);

  LIST_FOREACH(des, &sections, active_link) {
    if (des->changed == 2) {
      des->callback(des->opaque, mt->mt_pid, ptr, len, emm);
      if (!emm) { /* ECM */
        if ((t = mt->mt_service) != NULL) {
          tvh_mutex_lock(&t->s_stream_mutex);
          /* The keys are requested from this moment */
          dr = t->s_descramble;
          if (dr) {
            if (!dr->dr_quick_ecm && !des->quick_ecm_called) {
              des->quick_ecm_called = 1;
              dr->dr_quick_ecm = descrambler_quick_ecm(mt->mt_service, mt->mt_pid);
              if (dr->dr_quick_ecm)
                tvhdebug(LS_DESCRAMBLER, "quick ECM enabled for service '%s'",
                         t->s_dvb_svcname);
            }
            if ((ptr[0] & 0xfe) == 0x80) { /* 0x80 = even, 0x81 = odd */
              j = ptr[0] & 1;
              if (dr->dr_ecm_parity == ECM_PARITY_81EVEN_80ODD)
                j ^= 1;
              dr->dr_ecm_start[j] = clk;
              ki = 1 << (j + 6); /* 0x40 = even, 0x80 = odd */
              for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
                tk = &dr->dr_keys[i];
                if (dr->dr_quick_ecm)
                  tk->key_valid &= ~ki;
                TAILQ_FOREACH(st, &mt->mt_service->s_components.set_filter, es_filter_link) {
                  if (st->es_pid != mt->mt_pid) continue;
                    LIST_FOREACH(ca, &st->es_caids, link) {
                    if (ca->use == 0) continue;
                    tk->key_csa.csa_ecm = (caid_is_videoguard(ca->caid) && (ptr[4] != 0 && (ptr[2] - ptr[4]) == 4)) ? 4 : 0;
                    tvhtrace(LS_DESCRAMBLER, "key ecm=%X (caid=%04X)", tk->key_csa.csa_ecm, ca->caid);
                  }
                }
                if (tk->key_pid == 0) break;
              }
            }
            tvhtrace(LS_DESCRAMBLER, "ECM message %02x:%02x (section %d, len %d, pid %d) for service \"%s\"",
                     ptr[0], ptr[1], des->number, len, mt->mt_pid, t->s_dvb_svcname);
          }
          tvh_mutex_unlock(&t->s_stream_mutex);
        } else
          tvhtrace(LS_DESCRAMBLER, "Unknown fast table message %02x (section %d, len %d, pid %d)",
                   ptr[0], des->number, len, mt->mt_pid);
      } else if (tvhtrace_enabled()) {
        const char *s;
        if (mt->mt_pid == DVB_PAT_PID)      s = "PAT";
        else if (mt->mt_pid == DVB_CAT_PID) s = "CAT";
        else                                s = "EMM";
        if (len >= 18)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7],
                   ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15],
                   ptr[16], ptr[17], len, mt->mt_pid);
        else if (len >= 6)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x:%02x:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], len, mt->mt_pid);
        else if (len >= 4)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], len, mt->mt_pid);
      }
    } else if (des->changed == 1 && !emm) {
      if ((t = mt->mt_service) != NULL) {
        tvh_mutex_lock(&t->s_stream_mutex);
        if ((dr = t->s_descramble) != NULL) {
          for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
            tk = &dr->dr_keys[i];
            for (j = 0; j < 2; j++) {
              clk2 = dr->dr_ecm_start[j];
              clk3 = tk->key_timestamp[j];
              if (clk3 > 0 && clk3 >= clk2 && clk3 + ms2mono(200) <= clk) {
                tk->key_timestamp[j] = clk;
                tvhtrace(LS_DESCRAMBLER, "ECM: %s key[%d] for service \"%s\" still valid",
                                         j == 0 ? "Even" : "Odd",
                                         tk->key_pid, t->s_dvb_svcname);
              }
            }
          }
        }
        tvh_mutex_unlock(&t->s_stream_mutex);
      }
    }
  }

  while ((des = LIST_FIRST(&sections)) != NULL) {
    LIST_REMOVE(des, active_link);
    descrambler_ecmsec_unref(des);
  }
  return 0;
}

static int
descrambler_open_pid_( mpegts_mux_t *mux, void *opaque, int pid,
                       descrambler_section_callback_t callback,
                       service_t *service )
{
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  int flags;

  if (mux == NULL)
    return 0;
  if (mux->mm_descrambler_flush)
    return 0;
  flags  = (pid >> 16) & MT_FAST;
  pid   &= 0x1fff;
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table->mt_pid != pid || (dt->table->mt_flags & MT_FAST) != flags)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link) {
      if (ds->opaque == opaque)
        return 0;
    }
    break;
  }
  if (!dt) {
    dt = calloc(1, sizeof(*dt));
    TAILQ_INIT(&dt->sections);
    dt->table = mpegts_table_add(mux, 0, 0, descrambler_table_callback,
                                 dt, (flags & MT_FAST) ? "ecm" : "emm",
                                 LS_TBL_CSA, MT_FULL | MT_DEFER | flags, pid,
                                 MPS_WEIGHT_CA);
    if (dt->table)
      dt->table->mt_service = (mpegts_service_t *)service;
    TAILQ_INSERT_TAIL(&mux->mm_descrambler_tables, dt, link);
  }
  ds = calloc(1, sizeof(*ds));
  ds->callback    = callback;
  ds->opaque      = opaque;
  LIST_INIT(&ds->ecmsecs);
  TAILQ_INSERT_TAIL(&dt->sections, ds, link);
  tvhtrace(LS_DESCRAMBLER, "mux %p open pid %04X (%i) (flags 0x%04x) for %p", mux, pid, pid, flags, opaque);
  return 1;
}

int
descrambler_open_pid( mpegts_mux_t *mux, void *opaque, int pid,
                      descrambler_section_callback_t callback,
                      service_t *service )
{
  int res;

  tvh_mutex_lock(&mux->mm_descrambler_lock);
  res = descrambler_open_pid_(mux, opaque, pid, callback, service);
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  return res;
}

static int
descrambler_close_pid_( mpegts_mux_t *mux, void *opaque, int pid )
{
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  int flags;

  if (mux == NULL)
    return 0;
  flags =  (pid >> 16) & MT_FAST;
  pid   &= 0x1fff;
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table->mt_pid != pid || (dt->table->mt_flags & MT_FAST) != flags)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link) {
      if (ds->opaque == opaque) {
        TAILQ_REMOVE(&dt->sections, ds, link);
        descrambler_destroy_section(ds, (flags & MT_FAST) == 0);
        if (TAILQ_FIRST(&dt->sections) == NULL) {
          TAILQ_REMOVE(&mux->mm_descrambler_tables, dt, link);
          descrambler_destroy_table_(dt);
        }
        tvhtrace(LS_DESCRAMBLER, "mux %p close pid %04X (%i) (flags 0x%04x) for %p", mux, pid, pid, flags, opaque);
        return 1;
      }
    }
  }
  return 0;
}

int
descrambler_close_pid( mpegts_mux_t *mux, void *opaque, int pid )
{
  int res;

  tvh_mutex_lock(&mux->mm_descrambler_lock);
  res = descrambler_close_pid_(mux, opaque, pid);
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  return res;
}

void
descrambler_flush_tables( mpegts_mux_t *mux )
{
  descrambler_table_t *dt;
  descrambler_emm_t *emm;

  if (mux == NULL)
    return;
  tvhtrace(LS_DESCRAMBLER, "mux %p - flush tables", mux);
  caclient_caid_update(mux, 0, 0, 0, -1);
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  mux->mm_descrambler_flush = 1;
  while ((dt = TAILQ_FIRST(&mux->mm_descrambler_tables)) != NULL) {
    TAILQ_REMOVE(&mux->mm_descrambler_tables, dt, link);
    descrambler_destroy_table(dt, (dt->table->mt_flags & MT_FAST) == 0);
  }
  while ((emm = TAILQ_FIRST(&mux->mm_descrambler_emms)) != NULL) {
    TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
    free(emm);
  }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
}

static void descrambler_cat_entry
  ( void *_mux, uint16_t caid, uint32_t prov, uint16_t pid )
{
  mpegts_mux_t *mux = _mux;
  descrambler_emm_t *emm;
  caclient_caid_update(mux, caid, prov, pid, 1);
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    if (emm->caid == caid && emm->prov == prov) {
      emm->to_be_removed = 0;
      if (emm->pid == EMM_PID_UNKNOWN) {
        tvhtrace(LS_DESCRAMBLER, "attach emm caid %04X (%i) prov %06X (%i) pid %04X (%i)",
                                 caid, caid, prov, prov, pid, pid);
        emm->pid = pid;
        descrambler_open_pid_(mux, emm->opaque, pid, emm->callback, NULL);
        break; // Only open a PID once
      }
    }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
}

static void descrambler_cat_clean( mpegts_mux_t *mux )
{
  descrambler_emm_t *emm;
  TAILQ_HEAD(,descrambler_emm) removing;
  uint16_t caid = 0, pid = 0;
  uint32_t prov;

  TAILQ_INIT(&removing);
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    if (emm->to_be_removed) {
      if (emm->pid != EMM_PID_UNKNOWN) {
        caid = emm->caid;
        prov = emm->prov;
        pid  = emm->pid;
        tvhtrace(LS_DESCRAMBLER, "close emm caid %04X (%i) prov %06X (%i) pid %04X (%i)",
                                 caid, caid, prov, prov, pid, pid);
        descrambler_close_pid_(mux, emm->opaque, pid);
      }
      TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
      TAILQ_INSERT_TAIL(&removing, emm, link);
    }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  while ((emm = TAILQ_FIRST(&removing)) != NULL) {
    if (emm->pid != EMM_PID_UNKNOWN)
      caclient_caid_update(mux, emm->caid, emm->prov, emm->pid, 0);
    TAILQ_REMOVE(&removing, emm, link);
    free(emm);
  }
}

void
descrambler_cat_data( mpegts_mux_t *mux, const uint8_t *data, int len )
{
  descrambler_emm_t *emm;

  tvhtrace(LS_DESCRAMBLER, "CAT data (len %d)", len);
  tvhlog_hexdump(LS_DESCRAMBLER, data, len);
  caclient_cat_update(mux, data, len);
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    emm->to_be_removed = 1;
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  dvb_cat_decode(data, len, descrambler_cat_entry, mux);
  descrambler_cat_clean(mux);
}

int
descrambler_open_emm( mpegts_mux_t *mux, void *opaque,
                      int caid, int prov,
                      descrambler_section_callback_t callback )
{
  descrambler_emm_t *emm;
  caid_t *c;
  int pid = EMM_PID_UNKNOWN;

  if (mux == NULL)
    return 0;
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  if (mux->mm_descrambler_flush)
    goto unlock;
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link) {
    if (emm->caid == caid && emm->prov == prov && emm->opaque == opaque) {
unlock:
      tvh_mutex_unlock(&mux->mm_descrambler_lock);
      return 0;
    }
  }
  emm = calloc(1, sizeof(*emm));
  emm->caid     = caid;
  emm->prov     = prov;
  emm->pid      = EMM_PID_UNKNOWN;
  emm->opaque   = opaque;
  emm->callback = callback;
  LIST_FOREACH(c, &mux->mm_descrambler_caids, link) {
    if (c->caid == caid && c->providerid == prov) {
      emm->pid = pid = c->pid;
      break;
    }
  }
  TAILQ_INSERT_TAIL(&mux->mm_descrambler_emms, emm, link);
  if (pid != EMM_PID_UNKNOWN) {
    tvhtrace(LS_DESCRAMBLER,
             "attach emm caid %04X (%i) pid %04X (%i) - direct",
             caid, caid, pid, pid);
    descrambler_open_pid_(mux, opaque, pid, callback, NULL);
  }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  return 1;
}

int
descrambler_close_emm( mpegts_mux_t *mux, void *opaque, int caid, int prov )
{
  descrambler_emm_t *emm;
  int pid;

  if (mux == NULL)
    return 0;
  tvh_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    if (emm->caid == caid && emm->prov == prov && emm->opaque == opaque)
      break;
  if (!emm) {
    tvh_mutex_unlock(&mux->mm_descrambler_lock);
    return 0;
  }
  TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
  pid  = emm->pid;
  if (pid != EMM_PID_UNKNOWN) {
    caid = emm->caid;
    prov = emm->prov;
    tvhtrace(LS_DESCRAMBLER, "close emm caid %04X (%i) prov %06X (%i) pid %04X (%i) - direct",
                             caid, caid, prov, prov, pid, pid);
    descrambler_close_pid_(mux, opaque, pid);
  }
  tvh_mutex_unlock(&mux->mm_descrambler_lock);
  free(emm);
  return 1;
}

/* **************************************************************************
 * Editor
 *
 * vim:sts=2:ts=2:sw=2:et
 * *************************************************************************/
