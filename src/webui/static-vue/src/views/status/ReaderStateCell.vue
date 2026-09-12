<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * ReaderStateCell — grid-cell renderer for CaReadersView's "State"
 * column. Maps the six th_descrambler_keystate_t strings emitted by
 * descrambler_keystate2str() (src/descrambler/descrambler.h /
 * descrambler.c keystatetab) to an icon + colour + plain-English
 * tooltip, same shape as configuration/CaStatusCell.vue (that one
 * covers the CA *client connection* state; this one covers a
 * per-service *descrambler instance*'s state - deliberately kept as
 * a separate, page-local component since the enum vocabularies
 * don't overlap).
 *
 *   INIT       → Circle       (muted,  "Initializing")
 *   READY      → CircleDot    (primary,"Warm standby") - actively
 *                querying ECM but not the one currently decrypting;
 *                with ECM race (config.descrambler_ecm_race) enabled
 *                this is where a warm, ready-to-fail-over reader sits.
 *   RESOLVED   → CircleCheck  (success,"Active") - currently decrypting.
 *   FORBIDDEN  → CircleAlert  (warning,"Access denied") - NOK from the
 *                card server.
 *   FATAL      → CircleX      (error,  "Fatal error")
 *   IDLE       → Circle       (muted,  "Idle") - parked (lost the
 *                "first key wins" race, ECM race disabled).
 */
import { computed, type Component } from 'vue'
import { Circle, CircleAlert, CircleCheck, CircleDot, CircleX } from 'lucide-vue-next'
import { t } from '@/composables/useI18n'

const props = defineProps<{ value: unknown }>()

interface StateEntry {
  icon: Component
  label: string
  cls: string
}

const STATES: Record<string, StateEntry> = {
  INIT: { icon: Circle, label: t('Initializing'), cls: 'reader-state--muted' },
  READY: { icon: CircleDot, label: t('Warm standby'), cls: 'reader-state--primary' },
  RESOLVED: { icon: CircleCheck, label: t('Active'), cls: 'reader-state--success' },
  FORBIDDEN: { icon: CircleAlert, label: t('Access denied'), cls: 'reader-state--warning' },
  FATAL: { icon: CircleX, label: t('Fatal error'), cls: 'reader-state--error' },
  IDLE: { icon: Circle, label: t('Idle'), cls: 'reader-state--muted' },
}

const state = computed<StateEntry | null>(() => {
  if (typeof props.value !== 'string') return null
  return STATES[props.value] ?? null
})
</script>

<template>
  <span v-if="state" v-tooltip.bottom="state.label" class="reader-state-wrap">
    <component
      :is="state.icon"
      :size="16"
      class="reader-state"
      :class="state.cls"
      :aria-label="state.label"
    />
    <span class="reader-state-text">{{ state.label }}</span>
  </span>
</template>

<style scoped>
.reader-state-wrap {
  display: inline-flex;
  align-items: center;
  gap: 6px;
}

.reader-state {
  flex: 0 0 auto;
}

.reader-state-text {
  white-space: nowrap;
}

.reader-state--muted {
  color: var(--tvh-text-muted);
}

.reader-state--primary {
  color: var(--tvh-primary);
}

.reader-state--success {
  color: var(--tvh-success);
}

.reader-state--warning {
  color: var(--tvh-warning);
}

.reader-state--error {
  color: var(--tvh-error);
}
</style>
