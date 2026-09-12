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
import StatusGrid from '@/components/StatusGrid.vue'
import ReaderStateCell from './ReaderStateCell.vue'
import StandbyReadyCell from './StandbyReadyCell.vue'
import FailoverCountCell from './FailoverCountCell.vue'
import type { ColumnDef } from '@/types/column'
import type { StatusEntry } from '@/stores/status'
import { useI18n } from '@/composables/useI18n'

const { t } = useI18n()

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
    class="status-view__grid"
  />
</template>

<style scoped>
.status-view__grid {
  flex: 1 1 auto;
  min-height: 0;
}
</style>
