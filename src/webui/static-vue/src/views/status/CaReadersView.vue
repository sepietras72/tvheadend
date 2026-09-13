<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * CaReadersView — Status > CA Readers tab. One row per (currently
 * subscribed/recorded service, CA client descrambling it) — "which
 * card server is actually serving this channel right now, and how
 * well".
 *
 * Backing endpoint api/status/ca_readers (ACCESS_ADMIN, api_status.c
 * api_status_ca_readers()). Read-only diagnostics — no toolbar
 * actions, so StatusGrid infers `selectable: false` and renders no
 * checkbox column (same inference ConnectionsView documents).
 *
 * No dedicated Comet notification class exists for this data (it
 * would need its own 1Hz backend timer). Reusing 'subscriptions' as
 * the trigger instead: subscription_status_callback() already fires
 * that class once a second for every active subscription, and CA
 * readers only exist for services something is actively subscribed
 * to - so the same heartbeat that keeps Subscriptions live is exactly
 * the right cadence and existence-scope for this view too, at zero
 * extra backend plumbing.
 *
 * ecm_ok/ecm_nok/ecm_min/avg/max/last come from th_descrambler_t
 * (descrambler.h - td_ecm_count/nok/time_*), accrued in
 * descrambler_notify() and on DS_FORBIDDEN transitions. Until now
 * only visible via LS_DESCRAMBLER log lines ("reader stats ...").
 *
 * last_error (td_ecm_last_error) is the human-readable reason for the
 * most recent failed ECM ("NOK: already has a key for service",
 * "Access denied (all ECM exhausted)", ...) - set in cclient.c
 * (cccam/newcamd) and capmt.c/capmt2.c (OSCam), cleared the moment the
 * next ECM succeeds - so it answers "what's wrong right now", not a
 * permanent log.
 */
import { computed } from 'vue'
import StatusGrid from '@/components/StatusGrid.vue'
import ReaderStateCell from './ReaderStateCell.vue'
import StandbyReadyCell from './StandbyReadyCell.vue'
import FailoverCountCell from './FailoverCountCell.vue'
import type { ColumnDef } from '@/types/column'
import { useStatusStore, type StatusEntry } from '@/stores/status'
import { useI18n } from '@/composables/useI18n'

const { t } = useI18n()

/*
 * Row highlighting (like OSCam's reader list): green for the reader
 * actually decrypting a service right now, blue for whichever reader
 * is CURRENTLY fastest for that SAME service - even when it isn't the
 * active one, so an admin can see at a glance whether the active
 * reader is really the best choice.
 *
 * bugfix: this compared ecm_avg (td_ecm_time_last's lifetime average
 * since the reader started, api_status.c) instead of ecm_last
 * (td_ecm_time_last, the single most recent round-trip). That's the
 * wrong metric - the real ECM-race failover logic this is supposed to
 * visualise, descrambler_maybe_switch_to_faster() in descrambler.c,
 * decides purely on td_ecm_time_last (see the long comment there on
 * why: td_ecm_time_last swings by orders of magnitude between
 * consecutive cycles of the very same reader, so an average would be
 * a sluggish, misleading indicator of "fastest right now"). Blue
 * must track the same number the failover mechanism itself acts on,
 * or it shows a reader as "fastest" that the app would never actually
 * switch to - reported by the user as "the blue doesn't match
 * reality".
 *
 * `useStatusStore` is cached at module scope by endpoint (see
 * stores/status.ts) - calling it again here with the exact same
 * (endpoint, notificationClass, keyField) StatusGrid uses below
 * returns the SAME store instance and entries array, not a second
 * fetch/Comet listener. That gives this view read access to every
 * row (needed to compare readers ACROSS a service) without
 * duplicating StatusGrid's data-fetching responsibility.
 */
const readersStore = useStatusStore<StatusEntry>('status/ca_readers', 'subscriptions', 'id')

/* id of the fastest (lowest ecm_last) reader per service, among
 * readers with at least one successful ECM - a reader with ecm_ok=0
 * has no timing to compare and never "wins" this. */
const fastestIdByService = computed<Set<unknown>>(() => {
  const bestLast = new Map<string, number>()
  const bestId = new Map<string, unknown>()
  for (const row of readersStore.entries) {
    const service = row.service
    const ok = row.ecm_ok
    const last = row.ecm_last
    if (typeof service !== 'string' || typeof ok !== 'number' || ok <= 0) continue
    if (typeof last !== 'number') continue
    const prev = bestLast.get(service)
    if (prev === undefined || last < prev) {
      bestLast.set(service, last)
      bestId.set(service, row.id)
    }
  }
  return new Set(bestId.values())
})

function rowClassFor(row: StatusEntry): string | undefined {
  if (row.keystate === 'RESOLVED') return 'ca-readers__row--active'
  if (fastestIdByService.value.has(row.id)) return 'ca-readers__row--fastest'
  return undefined
}

/* Combines ecm_min/avg/max/last into one "182 / 310 / 720 / 240 ms"
 * cell - same shape as the LS_DESCRAMBLER "reader stats" log line, so
 * an admin who's seen the log recognises the numbers immediately.
 * Kept out of individual sortable columns since sorting on any single
 * one of the four is rarely useful and four numeric columns for one
 * fact would crowd the grid. */
const fmtEcmTime = (_v: unknown, row: StatusEntry) => {
  const n = (f: string) => (typeof row[f] === 'number' ? (row[f] as number) : 0)
  if (n('ecm_ok') === 0) return ''
  return `${n('ecm_min')} / ${n('ecm_avg')} / ${n('ecm_max')} / ${n('ecm_last')} ms`
}

/*
 * Failover visualisation (descrambler_standby_promote(), descrambler.h
 * td_standby_valid/td_failover_count) - three icon/badge cells instead
 * of plain text, same visual language as configuration/CaStatusCell:
 *   - State (ReaderStateCell): coloured icon for keystate - green
 *     "Active" (currently decrypting), blue "Warm standby" (following
 *     ECM, ready but not active - where an ECM-race reader lives),
 *     grey "Idle" (parked), amber "Access denied", red "Fatal".
 *   - Standby (StandbyReadyCell): lit Zap icon = this reader holds a
 *     fresh cached key RIGHT NOW and could take over without a full
 *     ECM round-trip if the active reader died this instant.
 *   - Failovers (FailoverCountCell): badge when > 0 - how many times
 *     THIS reader has actually been promoted that way so far, i.e.
 *     evidence the mechanism has fired, not just that it's wired up.
 */

/*
 * 0 means unknown/unset rather than "CAID zero" - see td_caid in
 * descrambler.h. For capmt/capmt2 (OSCam) this is the first CAID
 * offered from the PMT for a multi-CAS service, not necessarily the
 * one OSCam ends up actually using internally (TVH doesn't see that
 * decision) - still far more useful than showing nothing for the
 * common single-CAS case. Hex, 4 digits, matches how CAIDs are
 * written everywhere else in Tvheadend (data/conf/descrambler, logs).
 */
const fmtCaid = (v: unknown) =>
  typeof v === 'number' && v > 0 ? v.toString(16).toUpperCase().padStart(4, '0') : '–'

/*
 * ecm_sent_ago (td_ecm_last_sent, descrambler.h) - ms since TVH last
 * ACTUALLY handed an ECM request to this reader's connection, -1 if
 * never. Deliberately independent of ecm_ok/ecm_avg (which only ever
 * reflect SUCCESSFUL replies) - this is the one column that still moves
 * for a reader stuck on a CAID that never answers, so "still trying" is
 * visible even when nothing has ever come back.
 */
const fmtSentAgo = (v: unknown) => {
  if (typeof v !== 'number' || v < 0) return t('never')
  if (v < 1000) return `${v} ms`
  return `${(v / 1000).toFixed(1)} s`
}

/*
 * ext (td_ext, descrambler.h) - whether this reader's connection
 * actually negotiated CCcam "EXT" (OSCam extension: multiple ECM
 * requests in flight at once) with the server, confirmed from the
 * server's own PARTNER: reply rather than local config. 0 covers both
 * "confirmed classic" and "not applicable" (capmt, old cccam) - both
 * read the same to an admin: this reader only ever has one ECM in
 * flight at a time, so ECM-race "keep warm" traffic is skipped on it.
 */
const fmtExt = (v: unknown) => (v === 1 ? t('Yes') : t('No'))

/*
 * last_error (td_ecm_last_error, descrambler.h) - a raw C string set
 * via snprintf in cclient.c/cccam2.c/cccam.c/capmt.c/capmt2.c, e.g.
 * "NOK: already has a key for service". Routed through t() so a
 * Polish (or any other) UI shows a real translation instead of the
 * backend's English text verbatim - see the matching msgid entries
 * added to intl/js/tvheadend.js.pl.po.
 *
 * One of those strings embeds two numbers ("NOK (seqno 5, 230 ms)")
 * that differ on every call, so it can't be a single translatable
 * msgid as-is - NOK_SEQNO_RE pulls the numbers back out and re-runs
 * them through t()'s {0}/{1} substitution against the templated
 * msgid "NOK (seqno {0}, {1} ms)" instead. Every other error string
 * is a fixed literal and goes through t() directly; any string with
 * no catalog entry (a future error text not yet translated) falls
 * back to the English original unchanged, same as everywhere else.
 */
const NOK_SEQNO_RE = /^NOK \(seqno (\d+), (\d+) ms\)$/
const fmtLastError = (v: unknown): string => {
  if (typeof v !== 'string' || !v) return ''
  const m = NOK_SEQNO_RE.exec(v)
  if (m) return t('NOK (seqno {0}, {1} ms)', m[1], m[2])
  return t(v)
}

/*
 * Phone-card layout: service as the bold headline (what you'd
 * recognise first - "which channel"), reader as the 2-up companion
 * ("which server"), ok/nok counts as the health-at-a-glance row.
 * ECM timing stays desktop-only - a diagnostic detail, not a
 * glance-worthy number.
 */
const cols: ColumnDef[] = [
  {
    field: 'service',
    label: t('Service'),
    sortable: true,
    minVisible: 'phone',
    phoneRole: 'primary',
  },
  {
    field: 'reader',
    label: t('CA Reader'),
    sortable: true,
    minVisible: 'phone',
    phoneOrder: 1,
  },
  {
    field: 'caid',
    label: t('CAID'),
    sortable: true,
    minVisible: 'desktop',
    format: fmtCaid,
  },
  {
    field: 'keystate',
    label: t('State'),
    sortable: true,
    minVisible: 'desktop',
    width: 150,
    cellComponent: ReaderStateCell,
  },
  { field: 'ecm_ok', label: t('ECM OK'), sortable: true, minVisible: 'phone', phoneOrder: 2 },
  { field: 'ecm_nok', label: t('ECM NOK'), sortable: true, minVisible: 'phone', phoneOrder: 3 },
  {
    /* td_ecm_last_error (descrambler.h) - cleared on the next
     * successful ECM, so this is "what's wrong right now", not a
     * permanent history log. Empty when the last response was fine. */
    field: 'last_error',
    label: t('Last ECM Error'),
    sortable: true,
    minVisible: 'desktop',
    width: 240,
    format: fmtLastError,
  },
  {
    field: 'ecm_avg',
    label: t('ECM Time (min/avg/max/last)'),
    sortable: true,
    minVisible: 'desktop',
    width: 220,
    format: fmtEcmTime,
  },
  {
    field: 'ecm_sent_ago',
    label: t('Last ECM Sent'),
    sortable: true,
    minVisible: 'desktop',
    width: 130,
    format: fmtSentAgo,
  },
  {
    field: 'ext',
    label: t('EXT'),
    sortable: true,
    minVisible: 'desktop',
    width: 70,
    format: fmtExt,
  },
  {
    field: 'standby_ready',
    label: t('Standby'),
    sortable: true,
    minVisible: 'desktop',
    width: 90,
    cellComponent: StandbyReadyCell,
  },
  {
    field: 'failover_count',
    label: t('Failovers'),
    sortable: true,
    minVisible: 'desktop',
    width: 110,
    cellComponent: FailoverCountCell,
  },
]
</script>

<template>
  <StatusGrid
    endpoint="status/ca_readers"
    notification-class="subscriptions"
    :columns="cols"
    key-field="id"
    :default-sort="{ key: 'service', dir: 'ASC' }"
    storage-key="status-ca-readers"
    :row-class="rowClassFor"
    class="status-view__grid"
  />
</template>

<style scoped>
.status-view__grid {
  flex: 1 1 auto;
  min-height: 0;
}
</style>

<!--
  Unscoped, like UpcomingView's dedup-skipped dimming: PrimeVue renders
  the DataTable's <tr>/phone card outside this component's scoped CSS
  boundary, so `scoped` + `:deep()` would work too but the project's
  established pattern for row-state colouring is a plain global block
  (see UpcomingView.vue "upcoming__row--skipped").
-->
<style>
tr.ca-readers__row--active > td {
  background: color-mix(in srgb, var(--tvh-success) 14%, transparent);
}

tr.ca-readers__row--fastest > td {
  background: color-mix(in srgb, var(--tvh-primary) 12%, transparent);
}

.data-grid__card.ca-readers__row--active {
  border-left: 3px solid var(--tvh-success);
}

.data-grid__card.ca-readers__row--fastest {
  border-left: 3px solid var(--tvh-primary);
}
</style>
