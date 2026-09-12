/*
 *  API - service related calls
 *
 *  Copyright (C) 2013 Adam Sutton
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

#ifndef __TVH_API_SERVICE_H__
#define __TVH_API_SERVICE_H__

#include "tvheadend.h"
#include "subscriptions.h"
#include "access.h"
#include "api.h"
#include "tcp.h"
#include "input.h"
#include "descrambler/descrambler.h"
#include "epggrab.h"  //Needed to get the next EPG grab times
#include "dvr/dvr.h"  //Needed to get the next schedule dvr time

static int
api_status_inputs
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int c = 0;
  htsmsg_t *l, *e;
  tvh_input_t *ti;
  tvh_input_stream_t *st;
  tvh_input_stream_list_t stl = { 0 };
  
  tvh_mutex_lock(&global_lock);
  TVH_INPUT_FOREACH(ti)
    ti->ti_get_streams(ti, &stl);
  tvh_mutex_unlock(&global_lock);

  l = htsmsg_create_list();
  while ((st = LIST_FIRST(&stl))) {
    e = tvh_input_stream_create_msg(st);
    htsmsg_add_msg(l, NULL, e);
    tvh_input_stream_destroy(st);
    LIST_REMOVE(st, link);
    free(st);
    c++;
  }
    
  *resp = htsmsg_create_map();
  htsmsg_add_msg(*resp, "entries", l);
  htsmsg_add_u32(*resp, "totalCount", c);

  return 0;
}

static int
api_status_subscriptions
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int c;
  htsmsg_t *l, *e;
  th_subscription_t *ths;

  l = htsmsg_create_list();
  c = 0;
  tvh_mutex_lock(&global_lock);
  LIST_FOREACH(ths, &subscriptions, ths_global_link) {
    e = subscription_create_msg(ths, perm->aa_lang_ui);
    htsmsg_add_msg(l, NULL, e);
    c++;
  }
  tvh_mutex_unlock(&global_lock);

  *resp = htsmsg_create_map();
  htsmsg_add_msg(*resp, "entries", l);
  htsmsg_add_u32(*resp, "totalCount", c);

  return 0;
}

/*
 * nowosc: podglad "kto opisuje/odszyfrowuje co" - jeden wiersz per
 * (usluga aktualnie ogladana/nagrywana, klient CA obslugujacy ja).
 * Dotad metryki liczone w descrambler.c (td_ecm_count/nok/time_*,
 * patrz descrambler.h) byly widoczne tylko w logu - to domyka je w
 * UI (Status -> CA Readers).
 *
 * Enumerujemy przez subskrypcje (nie przez wszystkie skonfigurowane
 * uslugi) - interesuja nas tylko czytnicy obslugujacy cos, co ktos
 * faktycznie w tej chwili oglada/nagrywa. Jedna usluga moze miec
 * wielu subskrybentow, wiec odrzucamy duplikaty po wskazniku service_t.
 *
 * "id" w odpowiedzi to adres wskaznika th_descrambler_t jako string -
 * te obiekty nie maja wlasnego UUID, a potrzebny jest stabilny klucz
 * do StatusGrid (Vue) na czas zycia czytnika.
 */
static int
api_status_ca_readers
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  int c = 0, i, nseen = 0, dup;
  htsmsg_t *l, *e;
  th_subscription_t *ths;
  service_t *t, *seen[256];
  th_descrambler_t *td;
  char idbuf[24];

  l = htsmsg_create_list();

  tvh_mutex_lock(&global_lock);
  LIST_FOREACH(ths, &subscriptions, ths_global_link) {
    t = ths->ths_service;
    if (t == NULL) continue;

    dup = 0;
    for (i = 0; i < nseen; i++)
      if (seen[i] == t) { dup = 1; break; }
    if (dup) continue;
    if (nseen < (int)ARRAY_SIZE(seen))
      seen[nseen++] = t;

    tvh_mutex_lock(&t->s_stream_mutex);
    LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
      snprintf(idbuf, sizeof(idbuf), "%p", (void *)td);
      e = htsmsg_create_map();
      htsmsg_add_str(e, "id", idbuf);
      /* nowosc: plain nazwa uslugi (np. "Eurosport 4 HD"), nie
         s_nicename ktore sklada "Siec/Mux/Usluga" - to drugie jest
         przydatne w logach, ale za dlugie/nieczytelne w tej kolumnie */
      htsmsg_add_str(e, "service", ((mpegts_service_t *)t)->s_dvb_svcname ?: "");
      /* nowosc: przyjazna nazwa klienta (Configuration -> Conditional
         Access "Client name") zamiast td_nicename (adres+port+CAID) -
         nie kazdy backend jeszcze go ustawia, wiec td_nicename to
         zapasowa wartosc, a nie puste pole */
      htsmsg_add_str(e, "reader",
                     (td->td_client_name && *td->td_client_name) ?
                       td->td_client_name : (td->td_nicename ?: ""));
      /* nowosc: CAID osobno (0 = nieznane/nie-per-CAID backend, patrz
         td_caid w descrambler.h) - format hex w UI */
      htsmsg_add_u32(e, "caid", td->td_caid);
      htsmsg_add_str(e, "keystate", descrambler_keystate2str(td->td_keystate));
      htsmsg_add_u32(e, "ecm_ok", td->td_ecm_count);
      htsmsg_add_u32(e, "ecm_nok", td->td_ecm_nok);
      htsmsg_add_u32(e, "ecm_min", td->td_ecm_time_min);
      htsmsg_add_u32(e, "ecm_avg", td->td_ecm_count ?
                     (uint32_t)(td->td_ecm_time_sum / td->td_ecm_count) : 0);
      htsmsg_add_u32(e, "ecm_max", td->td_ecm_time_max);
      htsmsg_add_u32(e, "ecm_last", td->td_ecm_time_last);
      /*
       * nowosc: widok failovera. standby_ready = TERAZ ma gotowy, swiezy
       * klucz zapasowy (td_standby_valid != 0) - gdyby aktywny czytnik
       * padl w tej chwili, ten moze przejac natychmiast. failover_count
       * = ile razy TEN czytnik faktycznie zostal tak promowany do tej
       * pory (patrz descrambler_standby_promote()) - dowod, ze
       * mechanizm dziala, nie tylko ze jest wlaczony.
       */
      htsmsg_add_u32(e, "standby_ready", td->td_standby_valid ? 1 : 0);
      htsmsg_add_u32(e, "failover_count", td->td_failover_count);
      /* nowosc: powod ostatniej nieudanej odpowiedzi ECM (pusty = brak
         biezacego bledu, ostatnia byla udana) - patrz td_ecm_last_error
         w descrambler.h */
      htsmsg_add_str(e, "last_error", td->td_ecm_last_error);
      /*
       * nowosc: "ile milisekund temu" TVH ostatni raz FAKTYCZNIE przekazal
       * zadanie ECM do polaczenia z serwerem (td_ecm_last_sent,
       * descrambler.h - ustawiane np. w cccam2_send_ecm() po udanym
       * cccam2_send_msg()). -1 = jeszcze nigdy nic nie wyslano tym
       * czytnikiem. W odroznieniu od ecm_last/ecm_avg (ktore mowia tylko
       * o UDANYCH odpowiedziach) to dziala rowniez dla readera, ktory
       * regularnie probuje, ale nigdy nie dostaje odpowiedzi (np. utkniety
       * na CAID, ktory milczy) - bez tego taki przypadek wygladal w UI
       * identycznie jak reader, ktory w ogole przestal cokolwiek probowac.
       */
      htsmsg_add_s64(e, "ecm_sent_ago",
                     td->td_ecm_last_sent ?
                       (int64_t)((mclk() - td->td_ecm_last_sent) / 1000LL) : -1);
      htsmsg_add_msg(l, NULL, e);
      c++;
    }
    tvh_mutex_unlock(&t->s_stream_mutex);
  }
  tvh_mutex_unlock(&global_lock);

  *resp = htsmsg_create_map();
  htsmsg_add_msg(*resp, "entries", l);
  htsmsg_add_u32(*resp, "totalCount", c);

  return 0;
}

static int
api_status_connections
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  tvh_mutex_lock(&global_lock);
  *resp = tcp_server_connections();
  tvh_mutex_unlock(&global_lock);
  return 0;
}

static int
api_connections_cancel
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  htsmsg_field_t *f;
  htsmsg_t *ids;
  uint32_t id;
  const char *s;

  if (!(f = htsmsg_field_find(args, "id")))
    return EINVAL;
  s = htsmsg_field_get_str(f);
  if (s && strcmp(s, "all") == 0) {
    tvh_mutex_lock(&global_lock);
    tcp_connection_cancel_all();
    tvh_mutex_unlock(&global_lock);
    return 0;
  }
  if (!(ids = htsmsg_field_get_list(f)))
    if (htsmsg_field_get_u32(f, &id))
      return EINVAL;

  if (ids) {
    HTSMSG_FOREACH(f, ids) {
      if (htsmsg_field_get_u32(f, &id)) continue;
      if (!id) continue;
      tvh_mutex_lock(&global_lock);
      tcp_connection_cancel(id);
      tvh_mutex_unlock(&global_lock);
    }
  } else {
    tvh_mutex_lock(&global_lock);
    tcp_connection_cancel(id);
    tvh_mutex_unlock(&global_lock);
  }
  return 0;
}

static void
input_clear_stats(const char *uuid)
{
  tvh_input_instance_t *tii;
  tvh_input_t *ti;

  tvh_mutex_lock(&global_lock);
  if ((tii = tvh_input_instance_find_by_uuid(uuid)) != NULL)
    if (tii->tii_clear_stats)
      tii->tii_clear_stats(tii);
  if ((ti = tvh_input_find_by_uuid(uuid)) != NULL)
    if (ti->ti_clear_stats)
      ti->ti_clear_stats(ti);
  tvh_mutex_unlock(&global_lock);
}

static int
api_status_input_clear_stats
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  htsmsg_field_t *f;
  htsmsg_t *ids;
  const char *uuid;

  if (!(f = htsmsg_field_find(args, "uuid")))
    return EINVAL;
  if (!(ids = htsmsg_field_get_list(f))) {
    if ((uuid = htsmsg_field_get_str(f)) == NULL)
      return EINVAL;
    input_clear_stats(uuid);
  } else {
    HTSMSG_FOREACH(f, ids) {
      if ((uuid = htsmsg_field_get_str(f)) == NULL) continue;
      input_clear_stats(uuid);
    }
  }
  return 0;
}

static int
api_status_activity
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  htsmsg_t *cats;
  time_t temp_earliest = 0;
  time_t temp_dvr = 0;
  time_t temp_ota = 0;
  time_t temp_int = 0;
  time_t temp_mux = 0;
  th_subscription_t *ths;
  uint32_t subscriptionCount = 0;

  temp_dvr = dvr_entry_find_earliest();

  //Only evaluate the OTA grabber cron if there are active OTA modules.
  if(epggrab_count_type(EPGGRAB_OTA))
  {
    temp_ota = epggrab_get_next_ota();
  }
  
  //Only evaluate the internal grabber cron if there are active internal modules.
  if(epggrab_count_type(EPGGRAB_INT))
  {
    temp_int = epggrab_get_next_int();
  }
  
  temp_mux = mpegts_mux_sched_next();

  temp_earliest = temp_dvr;

  if(temp_ota && ((temp_ota < temp_earliest) || (temp_earliest == 0)))
  {
    temp_earliest = temp_ota;
  }

  if(temp_int && ((temp_int < temp_earliest) || (temp_earliest == 0)))
  {
    temp_earliest = temp_int;
  }

  if(temp_mux && ((temp_mux < temp_earliest) || (temp_earliest == 0)))
  {
    temp_earliest = temp_mux;
  }

  cats = htsmsg_create_map();
  htsmsg_add_s64(cats, "dvr", temp_dvr);
  htsmsg_add_s64(cats, "ota_grabber", temp_ota);
  htsmsg_add_s64(cats, "int_grabber", temp_int);
  htsmsg_add_s64(cats, "mux_scheduler", temp_mux);

  tvh_mutex_lock(&global_lock);
  LIST_FOREACH(ths, &subscriptions, ths_global_link) {
    subscriptionCount++;
  }
  tvh_mutex_unlock(&global_lock);

  *resp = htsmsg_create_map();
  htsmsg_add_s64(*resp, "current_time", gclk());
  htsmsg_add_s64(*resp, "next_activity", temp_earliest);
  htsmsg_add_msg(*resp, "activities", cats);
  htsmsg_add_u32(*resp, "subscription_count", subscriptionCount);
  htsmsg_add_u32(*resp, "connection_count", tcp_server_connections_count());

  return 0;

}

void api_status_init ( void )
{
  static api_hook_t ah[] = {
    { "status/connections",   ACCESS_ADMIN, api_status_connections, NULL },
    { "status/subscriptions", ACCESS_ADMIN, api_status_subscriptions, NULL },
    { "status/ca_readers",    ACCESS_ADMIN, api_status_ca_readers, NULL },
    { "status/inputs",        ACCESS_ADMIN, api_status_inputs, NULL },
    { "status/inputclrstats", ACCESS_ADMIN, api_status_input_clear_stats, NULL },
    { "status/activity",      ACCESS_ADMIN, api_status_activity, NULL },
    { "connections/cancel",   ACCESS_ADMIN, api_connections_cancel, NULL },
    { NULL },
  };

  api_register_all(ah);
}


#endif /* __TVH_API_IDNODE_H__ */
