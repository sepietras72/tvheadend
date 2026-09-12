/*
 *  tvheadend, CCCAM2 interface - druga, niezalezna implementacja klienta
 *  CCcam, obok oryginalnego cccam.c (ktory zostaje nietkniety - dokladnie
 *  ten sam wzorzec co capmt.c/capmt2.c).
 *
 *  Ten plik to kopia cccam.c, w ktorej wszystkie rozbieznosci z
 *  zachowaniem prawdziwego OSCam (module-cccam.c, zrodla w /root/oscam)
 *  znalezione podczas sesji debugowania sa naprawione OD RAZU, a nie
 *  jako pozniejsze łatki:
 *   - nodeid: bajt "Partner ID" = 0x10 (prawdziwy OSCam), nie 'T'/'O'
 *     (module-cccam.c: cc_update_nodeid())
 *   - CLI_DATA: prawdziwy numeryczny numer builda (np. "3367"), nie
 *     tekstowy identyfikator (module-cccam.c: cc_send_cli_data())
 *   - PARTNER: prawdziwy format "OSCam %s (%s) [...]" (CS_VERSION/
 *     CS_TARGET), NIE fikcyjna wersja/build CCcam z loginu; i
 *     odpowiadamy na PARTNER: nawet gdy to serwer odezwie sie pierwszy
 *     (module-cccam.c: obsluga NOK1/NOK2, "if (!is_oscam_cccam) ...")
 *   - martwe polaczenie: po 12s calkowitej ciszy (ten sam prog co
 *     DEFAULT_CC_RECONNECT w prawdziwym OSCam, globals.h) wymuszamy
 *     PELNY reconnect (rozlacz+polacz+login), a nie tylko czyszczenie
 *     lokalnej flagi "busy" i dalsze probowanie na tym samym, mozliwie
 *     martwym na poziomie TCP gniezdzie (module-cccam.c:
 *     cc_request_timeout()+cc_cycle_connection())
 *
 *  Reszta (uwierzytelnianie, szyfrowanie RC4-podobne, format ramek i
 *  ECM_REQUEST/EMM_REQUEST) byla juz zweryfikowana bajt-po-bajcie jako
 *  identyczna z prawdziwym protokolem CCcam/OSCam - bez zmian.
 *
 *  tvheadend, CCCAM interface
 *  Copyright (C) 2007 Andreas Öman
 *  Copyright (C) 2017 Luis Alves
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

#include <ctype.h>
#include <openssl/sha.h>
#include "tvheadend.h"
#include "tcp.h"
#include "cclient.h"

/**
 *
 */
#define CCCAM_KEEPALIVE_INTERVAL  0
#define CCCAM_NETMSGSIZE          1024


typedef enum {
  MSG_CLI_DATA,         // client -> server
  MSG_ECM_REQUEST,      // client -> server
  MSG_EMM_REQUEST,      // client -> server
  MSG_CARD_REMOVED = 4, // server -> client
  MSG_CMD_05,
  MSG_KEEPALIVE,        // client -> server
  MSG_NEW_CARD,         // server -> client
  MSG_SRV_DATA,         // server -> client
  MSG_CMD_0A = 0x0a,
  MSG_CMD_0B = 0x0b,
  MSG_CMD_0C = 0x0c,    // CCcam 2.2.x fake client checks
  MSG_CMD_0D = 0x0d,    // "
  MSG_CMD_0E = 0x0e,    // "
  MSG_NEW_CARD_SIDINFO = 0x0f,
  MSG_SLEEPSEND = 0x80, // Sleepsend support
  MSG_ECM_NOK1 = 0xfe,  // server -> client ecm queue full, card not found
  MSG_ECM_NOK2 = 0xff,  // server -> client
  MSG_NO_HEADER = 0xffff
} cccam2_msg_type_t;

typedef enum {
  CCCAM2_EXTENDED_NONE = 0,
  CCCAM2_EXTENDED_EXT = 1,
} cccam2_extended_t;

typedef enum {
  CCCAM2_VERSION_2_0_11 = 0,
  CCCAM2_VERSION_2_1_1,
  CCCAM2_VERSION_2_1_2,
  CCCAM2_VERSION_2_1_3,
  CCCAM2_VERSION_2_1_4,
  CCCAM2_VERSION_2_2_0,
  CCCAM2_VERSION_2_2_1,
  CCCAM2_VERSION_2_3_0,
  CCCAM2_VERSION_COUNT,
} cccam2_version_t;

static const char *cccam2_version_str[CCCAM2_VERSION_COUNT] = {
  "2.0.11", "2.1.1", "2.1.2", "2.1.3",
  "2.1.4",  "2.2.0", "2.2.1", "2.3.0",
};

static const char *cccam2_build_str[CCCAM2_VERSION_COUNT] = {
  "2892",   "2971",  "3094",  "3165",
  "3191",   "3290",  "3316",  "3367",
};

/**
 *
 */
struct cccam2_crypt_block {
  uint8_t keytable[256];
  uint8_t state;
  uint8_t counter;
  uint8_t sum;
};

/**
 *
 */
typedef struct cccam2 {
  cclient_t;

  /* From configuration */
  uint8_t cccam_nodeid[8];
  int cccam_extended_conf;
  int cccam_extended;
  int cccam_version;

  uint8_t cccam_oscam;
  uint8_t cccam_sendsleep;
  uint8_t cccam_cansid;

  uint8_t cccam_busy;
  /*
   * nowosc: mclk() w momencie ustawienia cccam_busy - patrz
   * cccam2_set_busy() nizej. Bez tego, jesli serwer w ogole NIE
   * ODPOWIE na zadanie (nie NOK, nie klucz, ZERO odpowiedzi - inaczej
   * niz przypadki, ktore juz obslugujemy w cccam2_running_reply()),
   * flaga zostaje ustawiona NA ZAWSZE i polaczenie jest trwale
   * zablokowane - zaden z dotychczasowych fixow (PARTNER:, reset przy
   * loginie) nie pomaga, bo wszystkie zakladaja ze JAKAS odpowiedz w
   * koncu przyjdzie.
   */
  int64_t cccam_busy_since;

  struct cccam2_crypt_block sendblock;
  struct cccam2_crypt_block recvblock;

} cccam2_t;


static const uint8_t cccam2_str[] = "CCcam";

static void cccam2_send_oscam_extended(cccam2_t *cccam2);

/**
 *
 */
static inline const char *cccam2_get_version_str(cccam2_t *cccam2)
{
  int ver = MINMAX(cccam2->cccam_version, 0, ARRAY_SIZE(cccam2_version_str) - 1);
  return cccam2_version_str[ver];
}

/**
 *
 */
static inline const char *cccam2_get_build_str(cccam2_t *cccam2)
{
  int ver = MINMAX(cccam2->cccam_version, 0, ARRAY_SIZE(cccam2_version_str) - 1);
  return cccam2_build_str[ver];
}

/*
 * ile najdluzej czekamy na JAKAKOLWIEK odpowiedz (klucz, NOK, PARTNER:,
 * cokolwiek obslugiwane w cccam2_running_reply()) zanim uznamy
 * polaczenie za martwe. Ta sama wartosc (12s) co domyslny "cc_reconnect"
 * w prawdziwym OSCam (globals.h: DEFAULT_CC_RECONNECT 12000).
 */
#define CCCAM2_BUSY_TIMEOUT sec2mono(12)

/**
 *
 */
static inline int cccam2_set_busy(cccam2_t *cccam2)
{
  if (cccam2->cccam_extended)
    return 0;
  if (cccam2->cccam_busy) {
    if (mclk() - cccam2->cccam_busy_since < CCCAM2_BUSY_TIMEOUT)
      return 1;
    /*
     * prawdziwy OSCam w tej sytuacji NIE probuje dalej na TYM SAMYM
     * polaczeniu (cc_request_timeout()+cc_cycle_connection() w
     * module-cccam.c) - robi pelny cykl rozlacz+polacz, bo polaczenie
     * moze wygladac lokalnie na "aktywne" (fd otwarty) mimo ze jest
     * martwe na poziomie TCP (serwer padl bez FIN, NAT/firewall po
     * drodze cicho porzucil sesje). Wymuszamy wiec to samo, co juz
     * robi cc_thread() przy zmianie konfiguracji (cclient.c):
     * reconfigure=1 + shutdown(fd) - petla w cc_thread() wykryje to
     * (max ok. 1s) i sama zamknie i otworzy polaczenie na nowo.
     */
    tvhwarn(cccam2->cc_subsys,
            "%s: previous ECM request never got a reply (>%ds) - "
            "cycling connection", cccam2->cc_name,
            (int)(CCCAM2_BUSY_TIMEOUT / 1000000));
    cccam2->cccam_busy = 0;
    ((cclient_t *)cccam2)->cc_reconfigure = 1;
    if (((cclient_t *)cccam2)->cc_fd >= 0)
      shutdown(((cclient_t *)cccam2)->cc_fd, SHUT_RDWR);
    return 1;
  }
  cccam2->cccam_busy = 1;
  cccam2->cccam_busy_since = mclk();
  return 0;
}

/**
 *
 */
static inline void cccam2_unset_busy(cccam2_t *cccam2)
{
  cccam2->cccam_busy = 0;
}

/**
 *
 */
static inline void
uint8_swap(uint8_t *p1, uint8_t *p2)
{
  uint8_t tmp = *p1; *p1 = *p2; *p2 = tmp;
}

/**
 *
 */
static void
cccam2_crypt_xor(uint8_t *buf)
{
  uint8_t i;

  for (i = 0; i < 8; i++) {
    buf[i + 8] = i * buf[i];
    if (i <= 5)
      buf[i] ^= cccam2_str[i];
  }
}

/**
 *
 */
static void
cccam2_crypt_init(struct cccam2_crypt_block *block, uint8_t *key, int32_t len)
{
  uint32_t i = 0;
  uint8_t j = 0;

  for (i = 0; i < 256; i++) {
    block->keytable[i] = i;
  }
  for (i = 0; i < 256; i++) {
    j += key[i % len] + block->keytable[i];
    uint8_swap(&block->keytable[i], &block->keytable[j]);
  }
  block->state = *key;
  block->counter = 0;
  block->sum = 0;
}

/**
 *
 */
static void
cccam2_decrypt(struct cccam2_crypt_block *block, uint8_t *data, int32_t len)
{
  int32_t i;
  uint8_t z;

  for (i = 0; i < len; i++) {
    block->counter++;
    block->sum += block->keytable[block->counter];
    uint8_swap(&block->keytable[block->counter], &block->keytable[block->sum]);
    z = data[i];
    data[i] = z ^ block->keytable[(block->keytable[block->counter] +
              block->keytable[block->sum]) & 0xff] ^ block->state;
    z = data[i];
    block->state ^= z;
  }
}

/**
 *
 */
static void
cccam2_encrypt(struct cccam2_crypt_block *block, uint8_t *data, int32_t len)
{
  int32_t i;
  uint8_t z;
  for (i = 0; i < len; i++) {
    block->counter++;
    block->sum += block->keytable[block->counter];
    uint8_swap(&block->keytable[block->counter], &block->keytable[block->sum]);
    z = data[i];
    data[i] = z ^ block->keytable[(block->keytable[block->counter] +
              block->keytable[block->sum]) & 0xff] ^ block->state;
    block->state ^= z;
  }
}

static void
cccam2_decrypt_cw(uint8_t *nodeid, uint32_t card_id, uint8_t *cws)
{
  uint8_t tmp, i;
  uint64_t node_id = be64toh(*((uint64_t *) nodeid));

  for (i = 0; i < 16; i++) {
    tmp = cws[i] ^ (node_id >> (4 * i));
    if (i & 1)
      tmp = ~tmp;
    cws[i] = (card_id >> (2 * i)) ^ tmp;
  }
}

/**
 *
 */
static int
cccam2_oscam_check(cccam2_t *cccam2, uint8_t *buf)
{
  if (!cccam2->cccam_oscam) {
    uint16_t sum = 0x1234;
    uint16_t recv_sum = (buf[14] << 8) | buf[15];
    int32_t i;
    for (i = 0; i < 14; i++)
      sum += buf[i];
    tvhtrace(cccam2->cc_subsys, "%s: oscam check sum %04X recv sum %04X",
             cccam2->cc_name, sum, recv_sum);
    cccam2->cccam_oscam = sum == recv_sum;
    if (cccam2->cccam_oscam)
      tvhinfo(cccam2->cc_subsys, "%s: oscam server detected", cccam2->cc_name);
  }
  return cccam2->cccam_oscam;
}

/**
 *
 */
static int
cccam2_oscam_nodeid_check(cccam2_t *cccam2, uint8_t *buf)
{
  if (!cccam2->cccam_oscam) {
    uint16_t sum = 0x1234;
    uint16_t recv_sum = (buf[6] << 8) | buf[7];
    int32_t i;
    for (i = 0; i < 6; i++)
      sum += buf[i];
    tvhtrace(cccam2->cc_subsys, "%s: oscam nodeid check sum %04X recv sum %04X",
             cccam2->cc_name, sum, recv_sum);
    cccam2->cccam_oscam = sum == recv_sum;
    if (cccam2->cccam_oscam)
      tvhinfo(cccam2->cc_subsys, "%s: oscam server detected", cccam2->cc_name);
  }
  return cccam2->cccam_oscam;
}

/**
 *
 */
static inline uint8_t *
cccam2_set_ua(uint8_t *dst, uint8_t *src)
{
  /* FIXME */
  return memcpy(dst, src, 8);
}

/**
 *
 */
static inline uint8_t *
cccam2_set_sa(uint8_t *dst, uint8_t *src)
{
  return memcpy(dst, src, 4);
}

/**
 * Handle reply to card data request
 */
static int
cccam2_decode_card_data_reply(cccam2_t *cccam2, uint8_t *msg)
{
  cc_card_data_t *pcard;
  int i;
  unsigned int nprov;
  uint8_t **pid, **psa, *saa, *msg2, ua[8];

  /* nr of providers */
  nprov = msg[24];

  pid = nprov ? alloca(nprov * sizeof(uint8_t *)) : NULL;
  psa = nprov ? alloca(nprov * sizeof(uint8_t *)) : NULL;
  saa = nprov ? alloca(nprov * 8) : NULL;

  if (pid == NULL || psa == NULL || saa == NULL)
    return -ENOMEM;

  msg2 = msg + 25;
  memset(saa, 0, nprov * 8);
  for (i = 0; i < nprov; i++) {
    pid[i] = msg2;
    psa[i] = cccam2_set_sa(saa + i * 8, msg2 + 3);
    msg2 += 7;
  }

  caclient_set_status((caclient_t *)cccam2, CACLIENT_STATUS_CONNECTED);
  cccam2_set_ua(ua, msg + 16);
  pcard = cc_new_card((cclient_t *)cccam2, (msg[12] << 8) | msg[13],
                      (msg[4] << 24) | (msg[5] << 16) | (msg[6] << 8) | msg[7],
                      ua, nprov, pid, psa, 0);
  if (pcard) {
    /* pole "cccam" w unii cc_card_data_t (cclient.h) - wspolna infra,
     * nazwa pola sie nie zmienia dla cccam2 */
    pcard->cccam.cs_remote_id = (msg[8] << 24) | (msg[9] << 16) | (msg[10] << 8) | msg[11];
    pcard->cccam.cs_hop = msg[14];
    pcard->cccam.cs_reshare = msg[15];
  }

  return 0;
}

/**
 *
 */
static void
cccam2_handle_keys(cccam2_t *cccam2, cc_service_t *ct, cc_ecm_section_t *es,
                  uint8_t *buf, int len, int seq)
{
  uint8_t *dcw_even, *dcw_odd, _dcw[16];

  if (buf[1] == MSG_ECM_REQUEST) {
    if (!cccam2->cccam_extended) {
      cccam2_decrypt_cw(cccam2->cccam_nodeid, es->es_card_id, buf + 4);
      memcpy(_dcw, buf + 4, 16);
      cccam2_decrypt(&cccam2->recvblock, buf + 4, len - 4);
    } else {
      memcpy(_dcw, buf + 4, 16);
    }
    dcw_even = _dcw;
    dcw_odd  = _dcw + 8;
  } else {
    dcw_even = NULL;
    dcw_odd  = NULL;
  }

  cc_ecm_reply(ct, es, DESCRAMBLER_CSA_CBC, dcw_even, dcw_odd, seq);
}

/**
 *
 */
static void
cccam2_handle_partner(cccam2_t *cccam2, uint8_t *msg)
{
  char *saveptr;
  char *p;
  int had_param = 0;
  p = strtok_r((char *)msg, "[", &saveptr);
  while (p) {
    if ((p = strtok_r(NULL, ",]", &saveptr)) == NULL)
      break;
    had_param = 1;
    if (strncmp(p, "EXT", 3) == 0)
      cccam2->cccam_extended = 1;
    else if (strncmp(p, "SID", 3) == 0)
      cccam2->cccam_cansid = 1;
    else if (strncmp(p, "SLP", 3) == 0)
      cccam2->cccam_sendsleep = 1;
  }
  tvhinfo(cccam2->cc_subsys, "%s: server supports extended capabilities%s%s%s",
          cccam2->cc_name,
          cccam2->cccam_extended ? " EXT" : "",
          cccam2->cccam_cansid ? " SID" : "",
          cccam2->cccam_sendsleep ? " SLP" : "");
  /*
   * prawdziwy OSCam ZAWSZE odsyla wlasne "PARTNER: ..." przy PIERWSZYM
   * odebraniu takiej wiadomosci, niezaleznie od tego, ktora strona
   * odezwala sie pierwsza (module-cccam.c, obsluga NOK1/NOK2:
   * "if (!is_oscam_cccam) { is_oscam_cccam = 1; ...wyslij...}").
   * cccam_oscam pelni tu ta sama role - chroni przed odeslaniem
   * drugiej odpowiedzi, gdy to MY odezwalismy sie pierwsi.
   */
  if (had_param && !cccam2->cccam_oscam) {
    cccam2->cccam_oscam = 1;
    cccam2_send_oscam_extended(cccam2);
  }
}

/**
 * Handle running reply
 * cc_mutex is held
 */
static int
cccam2_running_reply(cccam2_t *cccam2, uint8_t *buf, int len)
{
  cc_service_t *ct;
  cc_ecm_section_t *es;
  uint32_t cardid;
  uint8_t seq;

  if (len < 4)
    return -1;

  tvhtrace(cccam2->cc_subsys, "%s: response msg type=%d, response",
           cccam2->cc_name, buf[1]);
  tvhlog_hexdump(cccam2->cc_subsys, buf, len);

  switch (buf[1]) {
    case MSG_NEW_CARD_SIDINFO:
    case MSG_NEW_CARD:
      tvhtrace(cccam2->cc_subsys, "%s: add card message received", cccam2->cc_name);
      cccam2_decode_card_data_reply(cccam2, buf);
      break;
    case MSG_CARD_REMOVED:
      if (len >= 8) {
        cardid = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
        tvhtrace(cccam2->cc_subsys, "%s: del card %08X message received", cccam2->cc_name, cardid);
        cc_remove_card_by_id((cclient_t *)cccam2, cardid);
      }
      break;
    case MSG_KEEPALIVE:
      tvhtrace(cccam2->cc_subsys, "%s: keepalive", cccam2->cc_name);
      break;
    case MSG_EMM_REQUEST:   /* emm ack */
      tvhtrace(cccam2->cc_subsys, "%s: EMM message ACK received", cccam2->cc_name);
      cccam2_unset_busy(cccam2);
      break;
    case MSG_SLEEPSEND:
      tvhtrace(cccam2->cc_subsys, "%s: Sleep send received", cccam2->cc_name);
      if (len >= 5) goto req;
      break;
    case MSG_ECM_NOK1:      /* retry */
    case MSG_ECM_NOK2:      /* decode failed */
      if (len >= 2 && len < 8) goto req;
      if (len > 5) {
        /* partner detection */
        if (len >= 12 && strncmp((char *)buf + 4, "PARTNER:", 8) == 0) {
          /*
           * bugfix: to byla JEDYNA sciezka w tym switchu obslugujaca
           * odpowiedz na nasze zadanie ECM (NOK1/NOK2 - w koncu to jego
           * case), ktora NIE wolala cccam2_unset_busy(). Gdy serwer
           * (np. przy klasycznym, nie-EXT CCcam, gdzie w locie moze
           * byc tylko JEDNO zadanie na raz - patrz cccam2_set_busy())
           * wysle informacje "PARTNER:" zamiast normalnej odpowiedzi
           * NOK/klucz na TO zadanie, flaga cccam_busy zostawala
           * ustawiona NA STALE - kazde kolejne zadanie ECM na tym
           * polaczeniu bylo od tej pory porzucane jako "server is
           * busy" (patrz cccam2_send_ecm()), bez zadnej szansy na
           * odzyskanie sie samo z siebie (ani retry z poprzedniej
           * poprawki, ani nic innego, nie moglo pomoc - polaczenie
           * bylo trwale zablokowane od strony TVH, nie serwera).
           */
          cccam2_handle_partner(cccam2, buf + 4);
          cccam2_unset_busy(cccam2);
        } else {
          goto req;
        }
      }
      break;
    //case MSG_CMD_05:      /* ? */
    case MSG_ECM_REQUEST: { /* request reply */
req:
      seq = cccam2->cccam_extended ? buf[0] : 1;
      es = cc_find_pending_section((cclient_t *)cccam2, seq, &ct);
      if (es)
        cccam2_handle_keys(cccam2, ct, es, buf, len, seq);
      cccam2_unset_busy(cccam2);
      return 0;
    }
    case MSG_SRV_DATA:
      if (len == 0x4c) {
        tvhinfo(cccam2->cc_subsys,
                "%s: CCcam server version %s nodeid=%02x%02x%02x%02x%02x%02x%02x%02x",
                cccam2->cc_name, buf + 12,
                buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11]);
        if (cccam2_oscam_nodeid_check(cccam2, buf + 4))
          cccam2_send_oscam_extended(cccam2);
      } else {
        tvhtrace(cccam2->cc_subsys, "%s: SRV_DATA: unknown length %d",
                 cccam2->cc_name, len);
      }
      break;
    case MSG_CLI_DATA:
      tvhinfo(cccam2->cc_subsys, "%s: CCcam server authentication completed",
              cccam2->cc_name);
      break;
    default:
      tvhwarn(cccam2->cc_subsys, "%s: Unknown message received",
              cccam2->cc_name);
      break;
  }
  return 0;
}

/**
 *
 */
static int
cccam2_read_message0(cccam2_t *cccam2, const char *state, sbuf_t *rbuf, int timeout)
{
  uint16_t msglen;
  uint8_t hdr[4];
  struct cccam2_crypt_block block;

  if (rbuf->sb_ptr < 4)
    return 0;
  block = cccam2->recvblock;
  memcpy(hdr, rbuf->sb_data, 4);
  cccam2_decrypt(&cccam2->recvblock, hdr, 4);
  msglen = (hdr[2] << 8) | hdr[3];
  if (rbuf->sb_ptr >= msglen + 4) {
    memcpy(rbuf->sb_data, hdr, 4);
    cccam2_decrypt(&cccam2->recvblock, rbuf->sb_data + 4, msglen);
    return msglen + 4;
  } else {
    cccam2->recvblock = block;
    return 0;
  }
}

/**
 *
 */
static int
cccam2_send_msg(cccam2_t *cccam2, cccam2_msg_type_t cmd,
               uint8_t *buf, size_t len, int enq,
               uint8_t seq, uint32_t card_id)
{
  cc_message_t *cm;
  uint8_t *netbuf;

  if (len + 4 > CCCAM_NETMSGSIZE)
    return -1;

  cm = malloc(sizeof(cc_message_t) + len + 4);
  if (cm == NULL)
    return -1;

  netbuf = cm->cm_data;
  if (cmd == MSG_NO_HEADER) {
    memcpy(netbuf, buf, len);
  } else {
    netbuf[0] = cccam2->cccam_extended ? seq : 0;
    netbuf[1] = cmd;
    netbuf[2] = len >> 8;
    netbuf[3] = len;
    if (buf)
      memcpy(netbuf + 4, buf, len);
    len += 4;
  }

  cccam2_encrypt(&cccam2->sendblock, cm->cm_data, len);
  cm->cm_len = len;
  /*
   * nowosc: jawne potwierdzenie w logu, ze TVH FAKTYCZNIE przekazal
   * zaszyfrowany pakiet ECM_REQUEST do kolejki zapisu polaczenia
   * (cc_write_message() w cclient.c - wspolny silnik, ktory sam
   * wykonuje write() do gniazda w petli cc_session()). Odrozniamy to
   * od loga "Sending ECM (PID..." w cc_table_input() (cclient.c),
   * ktory pojawia sie ZANIM w ogole wywolamy cc_send_ecm() - nie mowi
   * nic o tym, czy TA konkretna proba faktycznie dotarla az tutaj (np.
   * przy fallbacku na inny CAID po "access denied" chcemy miec
   * pewnosc, ze zadanie z nowym CAID zostalo naprawde wyslane, a nie
   * ze cos po drodze cicho je zgubilo - patrz np. przypadek CAID
   * 0B01 bez zadnej odpowiedzi po fallbacku z 1861).
   */
  if (cmd == MSG_ECM_REQUEST)
    tvhdebug(cccam2->cc_subsys,
             "%s: ECM request actually handed to server connection "
             "(seqno: %d, %u bytes)", cccam2->cc_name, seq, cm->cm_len);
  cc_write_message((cclient_t *)cccam2, cm, enq);

  return 0;
}

/**
 * Send keep alive
 */
static void
cccam2_send_ka(void *cc)
{
  cccam2_t *cccam2 = cc;
  uint8_t buf[4];

  buf[0] = 0;
  buf[1] = MSG_KEEPALIVE;
  buf[2] = 0;
  buf[3] = 0;

  tvhtrace(cccam2->cc_subsys, "%s: send keepalive", cccam2->cc_name);
  cccam2_send_msg(cccam2, MSG_NO_HEADER, buf, 4, 1, 0, 0);
}

/**
 * Send keep alive
 */
static void
cccam2_send_oscam_extended(cccam2_t *cccam2)
{
  char buf[256];
  tvhdebug(cccam2->cc_subsys, "%s: send oscam extended", cccam2->cc_name);
  /* prawdziwy OSCam wysyla tu "PARTNER: OSCam %s (%s) [...]" z
   * %s=CS_VERSION (np. "2.26.02-11945"), %s=CS_TARGET (domyslnie
   * doslownie "unknown" - globals.h) - NIE fikcyjnej wersji/builda
   * CCcam z loginu w formacie ktorego prawdziwy OSCam nigdy nie
   * generuje. */
  snprintf(buf, sizeof(buf), "PARTNER: OSCam %s (%s) [EXT,SID,SLP]",
           "1.20", "unknown");
  cccam2_send_msg(cccam2, MSG_ECM_NOK1, (uint8_t *)buf, strlen(buf) + 1, 1, 0, 0);
}

/**
 *
 */
static void
cccam2_sha1_make_login_key(cccam2_t *cccam2, uint8_t *buf)
{
  SHA_CTX sha1;
  uint8_t hash[SHA_DIGEST_LENGTH];

  cccam2_crypt_xor(buf);

  SHA1_Init(&sha1);
  SHA1_Update(&sha1, buf, 16);
  SHA1_Final(hash, &sha1);

  tvhdebug(cccam2->cc_subsys, "%s: sha1 hash", cccam2->cc_name);
  tvhlog_hexdump(cccam2->cc_subsys, hash, sizeof(hash));

  cccam2_crypt_init(&cccam2->recvblock, hash, sizeof(hash));
  cccam2_decrypt(&cccam2->recvblock, buf, 16);

  cccam2_crypt_init(&cccam2->sendblock, buf, 16);
  cccam2_decrypt(&cccam2->sendblock, hash, sizeof(hash));

  // send crypted hash to server
  cccam2_send_msg(cccam2, MSG_NO_HEADER, hash, sizeof(hash), 0, 0, 0);
}

/**
 * Login command
 */
static int
cccam2_send_login(cccam2_t *cccam2)
{
  uint8_t buf[20], data[20], *pwd;
  size_t l;

  if (cccam2->cc_username == NULL)
    return 1;

  l = MIN(strlen(cccam2->cc_username), 20);

  /* send username */
  memset(buf + l, 0, 20 - l);
  memcpy(buf, cccam2->cc_username, l);
  cccam2_send_msg(cccam2, MSG_NO_HEADER, buf, 20, 0, 0, 0);

  /* send password 'xored' with CCcam */
  memcpy(buf, cccam2_str, 5);
  buf[5] = 0;
  if (cccam2->cc_password && cccam2->cc_password[0]) {
    l = strlen(cccam2->cc_password);
    pwd = alloca(l + 1);
    strcpy((char *)pwd, cccam2->cc_password);
    cccam2_encrypt(&cccam2->sendblock, pwd, l);
  }
  cccam2_send_msg(cccam2, MSG_NO_HEADER, buf, 6, 0, 0, 0);

  tvhdebug(cccam2->cc_subsys, "%s: login response", cccam2->cc_name);
  if (cc_read((cclient_t *)cccam2, data, 20, 5000)) {
    tvherror(cccam2->cc_subsys, "%s: login failed, pwd ack not received", cccam2->cc_name);
    return -2;
  }

  cccam2_decrypt(&cccam2->recvblock, data, 20);
  if (memcmp(data, "CCcam", 5)) {
    tvherror(cccam2->cc_subsys, "%s: login failed, usr/pwd invalid", cccam2->cc_name);
    return -2;
  } else {
    tvhinfo(cccam2->cc_subsys, "%s: login succeeded", cccam2->cc_name);
  }

  /*
   * nowosc (zabezpieczenie): cccam_busy przetrwa reconnect (cccam2_t
   * zyje dluzej niz pojedyncze polaczenie TCP - patrz cc_thread() w
   * cclient.c), wiec gdyby cokolwiek innego (poza juz poprawionym
   * przypadkiem PARTNER: powyzej w cccam2_running_reply()) kiedys
   * zostawilo ja "zawieszona" na 1, swiezo zalogowane polaczenie NIGDY
   * by sie z tego samo nie podnioslo - wymuszamy tutaj czysty stan,
   * bo swiezo zalogowane polaczenie z definicji nie ma jeszcze zadnego
   * zadania w locie.
   */
  cccam2_unset_busy(cccam2);

  return 0;
}

/**
 *
 */
static void
cccam2_send_cli_data(cccam2_t *cccam2)
{
  const int32_t size = 20 + 8 + 6 + 26 + 4 + 28 + 1;
  uint8_t buf[size];

  memset(buf, 0, sizeof(buf));
  strncpy((char *)buf, cccam2->cc_username ?: "", 20);
  memcpy(buf + 20, cccam2->cccam_nodeid, 8);
  buf[28] = 0; // TODO: wantemus = 1;
  strncpy((char *)buf + 29, cccam2_get_version_str(cccam2), 31);
  /* to pole to prawdziwy, numeryczny numer builda (np. "3367", ten sam
   * co cccam2_build_str[] powyzej) - zweryfikowane wprost w zrodlach
   * OSCam (module-cccam.c: cc_send_cli_data() -> memcpy(buf+61,
   * rdr->cc_build, ...)), NIE tekstowy identyfikator klienta. */
  strncpy((char *)buf + 61, cccam2_get_build_str(cccam2), 7);
  cccam2_send_msg(cccam2, MSG_CLI_DATA, buf, size, 0, 0, 0);
}

/**
 *
 */
static void
cccam2_oscam_update_idnode(cccam2_t *cccam2)
{
  uint8_t p[8];
  uint16_t sum = 0x1234;
  int32_t i;

  memcpy(p, cccam2->cccam_nodeid, 4);
  /* prawdziwy OSCam uzywa tu bajtu 0x10 ("Partner ID" - zweryfikowane
   * w zrodlach OSCam, module-cccam.c: cc_update_nodeid(), komentarz
   * "Oscam 0x10, vPlugServer 0x11, Hadu 0x12, ..."). */
  p[4] = 0x10;
  p[5] = 0xaa;
  for (i = 0; i < 5; i++)
    p[5] ^= p[i];
  for (i = 0; i < 6; i++)
    sum += p[i];
  p[6] = sum >> 8;
  p[7] = sum;
  memcpy(cccam2->cccam_nodeid, p, 8);
}

/**
 *
 */
static int
cccam2_init_session(void *cc)
{
  cccam2_t *cccam2 = cc;
  uint8_t buf[256];
  int r;

  cccam2->cccam_oscam = 0;
  cccam2->cccam_extended = 0;
  cccam2->cccam_sendsleep = 0;
  cccam2->cccam_cansid = 0;

  /**
   * Get init seed
   */
  tvhtrace(cccam2->cc_subsys, "%s: init seed", cccam2->cc_name);
  if((r = cc_read(cc, buf, 16, 5000))) {
    tvhinfo(cccam2->cc_subsys, "%s: init error (no init seed received)", cccam2->cc_name);
    return -1;
  }

  /* check for oscam-cccam2 */
  cccam2_oscam_check(cccam2, buf);

  cccam2_sha1_make_login_key(cccam2, buf);

  /**
   * Login
   */
  if (cccam2_send_login(cccam2))
    return -1;

  cccam2_send_cli_data(cccam2);

  return 0;
}

/**
 *
 */
static int
cccam2_send_ecm(void *cc, cc_service_t *ct, cc_ecm_section_t *es,
               cc_card_data_t *pcard, const uint8_t *msg, int len)
{
  mpegts_service_t *t = (mpegts_service_t *)ct->td_service;
  cccam2_t *cccam2 = cc;
  uint8_t *buf;
  uint16_t caid, sid;
  uint32_t provid, card_id;
  int seq;

  if (len > 255) {
    tvherror(cccam2->cc_subsys, "%s: ECM too big (%d bytes)", cccam2->cc_name, len);
    return -1;
  }

  if (cccam2_set_busy(cccam2)) {
    tvhinfo(cccam2->cc_subsys, "%s: Ignore ECM request %02X (server is busy)",
            cccam2->cc_name, msg[0]);
    /*
     * nowosc: widoczne w Status -> CA Readers (td_ecm_last_error).
     * "busy" to NIE odpowiedz serwera - to TVH swiadomie NIE WYSYLA
     * tego zadania, bo serwer nie zglosil "EXT" (rozszerzonych
     * mozliwosci) przy logowaniu (patrz cccam2_set_busy() - dla EXT ten
     * warunek nigdy nie jest prawdziwy). Klasyczny (nie-EXT) CCcam
     * pozwala na TYLKO JEDNO zadanie ECM w locie na polaczenie na raz;
     * kolejne, nadchodzace zanim poprzednie dostanie odpowiedz, sa PO
     * PROSTU PORZUCANE (nie kolejkowane) - im wiecej ruchu ECM na tym
     * jednym polaczeniu (np. kilka kanalow na tym samym readerze, albo
     * wlaczony ECM race), tym wiecej takich odrzucen.
     */
    snprintf(((th_descrambler_t *)ct)->td_ecm_last_error,
             sizeof(((th_descrambler_t *)ct)->td_ecm_last_error),
             "Request dropped: server busy (no EXT, 1 request in flight)");
    return -1;
  }

  seq = atomic_add(&cccam2->cc_seq, 1);
  caid = es->es_caid;
  provid = es->es_provid;
  card_id = pcard->cs_id;
  es->es_card_id = card_id;
  sid = service_id16(t);
  /*
   * bugfix: cccam2_running_reply() (odbior odpowiedzi), dla polaczen BEZ
   * "EXT", zawsze uzywa STALEGO seq=1 do wyszukania czekajacej sekcji
   * (cc_find_pending_section) - bo klasyczny, nie-EXT protokol CCcam w
   * ogole nie niesie prawdziwego numeru sekwencyjnego na kablu (patrz
   * cccam2_send_msg(): "netbuf[0] = cccam_extended ? seq : 0"). Ten
   * kod jednak zapisywal na es->es_seq SUROWY, wciaz rosnacy licznik
   * (cccam2->cc_seq) NIEZALEZNIE od trybu - wiec dla KAZDEGO zadania po
   * pierwszym w calym zyciu polaczenia (es_seq=2,3,4,...) odpowiedz
   * serwera (szukana pod stalym seq=1) nigdy nie trafiala w
   * pending section. Efekt: "Got unexpected ECM reply (seqno: 1)",
   * klucz z tej odpowiedzi byl CICHO ODRZUCANY (cccam2_handle_keys()
   * nigdy nie zostawalo wywolane), a jedynym widocznym objawem bylo to,
   * ze polaczenie "nigdy nie dostaje odpowiedzi ECM" - mimo ze serwer
   * odpowiadal caly czas. Teraz dla nie-EXT wymuszamy es_seq=1, dokladnie
   * to samo, czego szuka strona odbiorcza.
   */
  es->es_seq = cccam2->cccam_extended ? (seq & 0xff) : 1;

  buf = alloca(len + 13);
  buf[ 0] = caid >> 8;
  buf[ 1] = caid & 0xff;
  buf[ 2] = provid >> 24;
  buf[ 3] = provid >> 16;
  buf[ 4] = provid >> 8;
  buf[ 5] = provid & 0xff;
  buf[ 6] = card_id >> 24;
  buf[ 7] = card_id >> 16;
  buf[ 8] = card_id >> 8;
  buf[ 9] = card_id & 0xff;
  buf[10] = sid >> 8;
  buf[11] = sid & 0xff;
  buf[12] = len;
  memcpy(buf + 13, msg, len);

  if (cccam2_send_msg(cccam2, MSG_ECM_REQUEST, buf, 13 + len, 1, seq, card_id))
    return -1;
  /*
   * nowosc: znacznik "ostatnio faktycznie wyslane" (td_ecm_last_sent,
   * descrambler.h) do Status -> CA Readers - patrz log "ECM request
   * actually handed to server connection" w cccam2_send_msg() powyzej,
   * to ten sam moment, tylko dostepny tu jako pole UI zamiast wpisu w
   * logu. Ustawiamy dopiero PO udanym cccam2_send_msg() (nie przy
   * wczesniejszym "server is busy" powyzej - tam nic faktycznie nie
   * wyslano).
   */
  ((th_descrambler_t *)ct)->td_ecm_last_sent = mclk();
  return 0;
}

/**
 *
 */
static void
cccam2_send_emm(void *cc, cc_service_t *ct, cc_card_data_t *pcard,
               uint32_t provid, const uint8_t *msg, int len)
{
  cccam2_t *cccam2 = cc;
  uint8_t *buf;
  uint16_t caid;
  uint32_t card_id;
  int seq;

  if (len > 255) {
    tvherror(cccam2->cc_subsys, "%s: EMM too big (%d bytes)",
             cccam2->cc_name, len);
    return;
  }

  if (cccam2_set_busy(cccam2)) {
    tvhinfo(cccam2->cc_subsys, "%s: Ignore EMM request %02X (server is busy)",
            cccam2->cc_name, msg[0]);
    return;
  }

  seq = atomic_add(&cccam2->cc_seq, 1);
  caid = pcard->cs_ra.caid;
  card_id = pcard->cs_id;

  buf = alloca(len + 12);
  buf[ 0] = caid >> 8;
  buf[ 1] = caid & 0xff;
  buf[ 2] = 0;
  buf[ 3] = provid >> 24;
  buf[ 4] = provid >> 16;
  buf[ 5] = provid >> 8;
  buf[ 6] = provid & 0xff;
  buf[ 7] = card_id >> 24;
  buf[ 8] = card_id >> 16;
  buf[ 9] = card_id >> 8;
  buf[10] = card_id & 0xff;
  buf[11] = len;
  memcpy(buf + 12, msg, len);

  cccam2_set_busy(cccam2);

  cccam2_send_msg(cccam2, MSG_EMM_REQUEST, buf, 12 + len, 1, seq, card_id);
}

/**
 *
 */
static int
cccam2_read(void *cc, sbuf_t *rbuf)
{
  cccam2_t *cccam2 = cc;
  const int ka_interval = cccam2->cc_keepalive_interval * 2 * 1000;
  int r;

  while (1) {
    r = cccam2_read_message0(cccam2, "Decoderloop", rbuf, ka_interval);
    if (r == 0)
      break;
    if (r < 0)
      return -1;
    if (r > 0) {
      int ret = cccam2_running_reply(cccam2, rbuf->sb_data, r);
      if (ret < 0)
        return -1;
      sbuf_cut(rbuf, r);
    }
  }
  return 0;
}

/**
 *
 */
static void
cccam2_no_services(void *cc)
{
  cccam2_unset_busy((cccam2_t *)cc);
}

/**
 *
 */
static void
cccam2_conf_changed(caclient_t *cac)
{
  cccam2_t *cccam2 = (cccam2_t *)cac;

  /**
   * Update nodeid for the oscam extended mode
   */
  if (cccam2->cccam_extended_conf == CCCAM2_EXTENDED_EXT)
    cccam2_oscam_update_idnode(cccam2);
  cc_conf_changed(cac);
}

/**
 *
 */
static int
nibble(char c)
{
  switch(c) {
  case '0' ... '9':
    return c - '0';
  case 'a' ... 'f':
    return c - 'a' + 10;
  case 'A' ... 'F':
    return c - 'A' + 10;
  default:
    return 0;
  }
}

/**
 *
 */
static int
caclient_cccam2_nodeid_set(void *o, const void *v)
{
  cccam2_t *cccam2 = o;
  const char *s = v ?: "";
  uint8_t key[8];
  int i, u, l;
  uint64_t node_id;

  for(i = 0; i < ARRAY_SIZE(key); i++) {
    while(*s != 0 && !isxdigit(*s)) s++;
    u = *s ? nibble(*s++) : 0;
    while(*s != 0 && !isxdigit(*s)) s++;
    l = *s ? nibble(*s++) : 0;
    key[i] = (u << 4) | l;
  }

  node_id = be64toh(*((uint64_t*) key));
  if (!node_id)
    uuid_random(key, 8);

  if ((i = memcmp(cccam2->cccam_nodeid, key, ARRAY_SIZE(key))) != 0)
    memcpy(cccam2->cccam_nodeid, key, ARRAY_SIZE(key));
  return i;
}

static const void *
caclient_cccam2_nodeid_get(void *o)
{
  cccam2_t *cccam2 = o;
  snprintf(prop_sbuf, PROP_SBUF_LEN,
           "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
           cccam2->cccam_nodeid[0x0],
           cccam2->cccam_nodeid[0x1],
           cccam2->cccam_nodeid[0x2],
           cccam2->cccam_nodeid[0x3],
           cccam2->cccam_nodeid[0x4],
           cccam2->cccam_nodeid[0x5],
           cccam2->cccam_nodeid[0x6],
           cccam2->cccam_nodeid[0x7]);
  return &prop_sbuf_ptr;
}

static htsmsg_t *
caclient_cccam2_class_cccam2_extended_list ( void *o, const char *lang )
{
  static const struct strtab tab[] = {
    { N_("None"), CCCAM2_EXTENDED_NONE },
    { N_("Oscam-EXT"), CCCAM2_EXTENDED_EXT },
  };
  return strtab2htsmsg(tab, 1, lang);
}

static htsmsg_t *
caclient_cccam2_class_cccam2_version_list ( void *o, const char *lang )
{
  static const struct strtab tab[] = {
    { N_("2.0.11"), CCCAM2_VERSION_2_0_11 },
    { N_("2.1.1"),  CCCAM2_VERSION_2_1_1 },
    { N_("2.1.2"),  CCCAM2_VERSION_2_1_2 },
    { N_("2.1.3"),  CCCAM2_VERSION_2_1_3 },
    { N_("2.1.4"),  CCCAM2_VERSION_2_1_4 },
    { N_("2.2.0"),  CCCAM2_VERSION_2_2_0 },
    { N_("2.2.1"),  CCCAM2_VERSION_2_2_1 },
    { N_("2.3.0"),  CCCAM2_VERSION_2_3_0 },
  };
  return strtab2htsmsg(tab, 1, lang);
}

const idclass_t caclient_cccam2_class =
{
  .ic_super      = &caclient_cc_class,
  .ic_class      = "caclient_cccam2",
  .ic_caption    = N_("CCcam2 (OSCam-compatible)"),
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "nodeid",
      .name     = N_("Node ID"),
      .desc     = N_("Client node ID. Leave field empty to generate a random ID."),
      .set      = caclient_cccam2_nodeid_set,
      .get      = caclient_cccam2_nodeid_get,
      .group    = 2,
    },
    {
      .type     = PT_INT,
      .id       = "extended",
      .name     = N_("Extended mode"),
      .desc     = N_("Extended mode settings."),
      .off      = offsetof(cccam2_t, cccam_extended_conf),
      .list     = caclient_cccam2_class_cccam2_extended_list,
      .def.i    = CCCAM2_EXTENDED_EXT,
      .opts     = PO_DOC_NLIST,
      .group    = 2,
    },
    {
      .type     = PT_INT,
      .id       = "version",
      .name     = N_("Version"),
      .desc     = N_("Protocol version."),
      .off      = offsetof(cccam2_t, cccam_version),
      .list     = caclient_cccam2_class_cccam2_version_list,
      .def.i    = CCCAM2_VERSION_2_3_0,
      .opts     = PO_DOC_NLIST,
      .group    = 2,
    },
    {
      .type     = PT_INT,
      .id       = "keepalive_interval",
      .name     = N_("Keepalive interval (0=disable)"),
      .desc     = N_("Keepalive interval in seconds"),
      .off      = offsetof(cccam2_t, cc_keepalive_interval),
      .def.i    = CCCAM_KEEPALIVE_INTERVAL,
      .group    = 4,
    },
    { }
  }
};

/*
 *
 */
caclient_t *cccam2_create(void)
{
  cccam2_t *cccam2 = calloc(1, sizeof(*cccam2));

  cccam2->cc_subsys = LS_CCCAM2;
  cccam2->cc_id     = "cccam2";

  tvh_mutex_init(&cccam2->cc_mutex, NULL);
  tvh_cond_init(&cccam2->cc_cond, 1);
  cccam2->cac_free         = cc_free;
  cccam2->cac_start        = cc_service_start;
  cccam2->cac_conf_changed = cccam2_conf_changed;
  cccam2->cac_caid_update  = cc_caid_update;
  cccam2->cc_keepalive_interval = CCCAM_KEEPALIVE_INTERVAL;
  cccam2->cccam_version    = CCCAM2_VERSION_2_3_0;
  cccam2->cc_init_session  = cccam2_init_session;
  cccam2->cc_read          = cccam2_read;
  cccam2->cc_send_ecm      = cccam2_send_ecm;
  cccam2->cc_send_emm      = cccam2_send_emm;
  cccam2->cc_keepalive     = cccam2_send_ka;
  cccam2->cc_no_services   = cccam2_no_services;
  return (caclient_t *)cccam2;
}
