<!--
  SPDX-License-Identifier: GPL-3.0-or-later
  Copyright (C) 2026 Tvheadend contributors
-->
<script setup lang="ts">
/*
 * FailoverCountCell — CaReadersView's "Failovers" column. Visualises
 * td_failover_count (descrambler.h): how many times THIS reader has
 * actually been promoted from cached standby to active
 * (descrambler_standby_promote(), descrambler.c) - concrete evidence
 * the fast-failover mechanism has fired, not just that it's wired up.
 * A pill badge when it's happened at least once (so a reader with a
 * failover history stands out scanning the column); a plain muted
 * dash at zero (nothing to see, not an error state).
 */
import { computed } from 'vue'
import { RotateCw } from 'lucide-vue-next'
import { t } from '@/composables/useI18n'

const props = defineProps<{ value: unknown }>()

const count = computed(() => (typeof props.value === 'number' ? props.value : 0))
</script>

<template>
  <span
    v-if="count > 0"
    v-tooltip.bottom="t('Promoted from standby {0} time(s)', count)"
    class="failover-badge"
  >
    <RotateCw :size="12" class="failover-badge-icon" />
    {{ count }}
  </span>
  <span v-else class="failover-none">–</span>
</template>

<style scoped>
.failover-badge {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 7px;
  border-radius: 999px;
  font-size: 0.85em;
  font-weight: 600;
  color: var(--tvh-primary);
  background: color-mix(in srgb, var(--tvh-primary) 14%, transparent);
}

.failover-badge-icon {
  flex: 0 0 auto;
}

.failover-none {
  color: var(--tvh-text-muted);
}
</style>
