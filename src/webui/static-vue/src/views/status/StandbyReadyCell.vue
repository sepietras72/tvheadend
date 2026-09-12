<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * StandbyReadyCell — CaReadersView's "Standby" column. Visualises
 * td_standby_valid (descrambler.h) as a Zap icon: lit when this
 * reader holds a fresh, cached key right now and could take over
 * instantly if the active reader died this instant
 * (descrambler_standby_promote(), descrambler.c); dimmed otherwise.
 * `value` is the API's standby_ready 0/1.
 */
import { computed } from 'vue'
import { Zap, ZapOff } from 'lucide-vue-next'
import { t } from '@/composables/useI18n'

const props = defineProps<{ value: unknown }>()

const ready = computed(() => props.value === 1)
const label = computed(() =>
  ready.value ? t('Standby key ready - can fail over instantly') : t('No standby key cached'),
)
</script>

<template>
  <span v-tooltip.bottom="label" class="standby-wrap">
    <component
      :is="ready ? Zap : ZapOff"
      :size="16"
      class="standby-icon"
      :class="ready ? 'standby-icon--ready' : 'standby-icon--none'"
      :aria-label="label"
    />
  </span>
</template>

<style scoped>
.standby-wrap {
  display: inline-flex;
  align-items: center;
}

.standby-icon--ready {
  color: var(--tvh-success);
}

.standby-icon--none {
  color: var(--tvh-text-muted);
  opacity: 0.5;
}
</style>
