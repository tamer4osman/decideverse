/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * games/index.js - GameLab game-module registration.
 *
 * Stage 2 registered The Last Detective as the first playable module;
 * Stage 3 adds Living Village. The remaining planned games are roadmap
 * scopes. Adding a game = one import + one registerGame() call.
 */

import { registerGame } from '../core/registry.js';
import { createDetectiveModule } from './detective/module.js';
import {
    DEFAULT_SEED, CASE_META
} from './detective/scenario.js';
import { createLivingVillageModule } from './living-village/module.js';
import { DEFAULT_SEED as VILLAGE_SEED } from './living-village/scenario.js';

const ICONS = {
    detective: `<svg viewBox="0 0 64 64" width="42" height="42" aria-hidden="true">
      <circle cx="32" cy="26" r="16" fill="none" stroke="currentColor" stroke-width="3"/>
      <circle cx="32" cy="26" r="7" fill="none" stroke="currentColor" stroke-width="2.4"/>
      <path d="M32 6v6M32 40v6M12 26h6M46 26h6" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>
      <path d="M20 54c4-6 8-9 12-9s8 3 12 9" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>
    </svg>`,
    village: `<svg viewBox="0 0 64 64" width="42" height="42" aria-hidden="true">
      <path d="M10 54V32l10-10 10 10v22zM34 54V26l10-8 10 8v28z" fill="none" stroke="currentColor" stroke-width="3" stroke-linejoin="round"/>
      <path d="M8 54h48" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>
      <circle cx="29" cy="20" r="4" fill="none" stroke="currentColor" stroke-width="2.4"/>
    </svg>`,
    tactical: `<svg viewBox="0 0 64 64" width="42" height="42" aria-hidden="true">
      <rect x="8" y="8" width="48" height="48" rx="6" fill="none" stroke="currentColor" stroke-width="3"/>
      <path d="M8 24h48M8 40h48M24 8v48M40 8v48" stroke="currentColor" stroke-width="2"/>
      <circle cx="16" cy="16" r="3" fill="currentColor"/><circle cx="48" cy="48" r="3" fill="currentColor"/>
    </svg>`,
    outpost: `<svg viewBox="0 0 64 64" width="42" height="42" aria-hidden="true">
      <path d="M14 52l6-30 12-8 12 8 6 30z" fill="none" stroke="currentColor" stroke-width="3" stroke-linejoin="round"/>
      <path d="M26 52V38h12v14" fill="none" stroke="currentColor" stroke-width="3"/>
      <path d="M32 14v-6" stroke="currentColor" stroke-width="3" stroke-linecap="round"/>
      <circle cx="32" cy="6" r="2.6" fill="currentColor"/>
    </svg>`,
    kingdoms: `<svg viewBox="0 0 64 64" width="42" height="42" aria-hidden="true">
      <path d="M12 48l-4-24 12 8 12-16 12 16 12-8-4 24z" fill="none" stroke="currentColor" stroke-width="3" stroke-linejoin="round"/>
      <path d="M12 48h40v8H12z" fill="none" stroke="currentColor" stroke-width="3" stroke-linejoin="round"/>
    </svg>`
};

registerGame({
    id: 'detective',
    name: 'The Last Detective',
    tagline: 'Evidence-driven mystery at the Aldermoor Museum',
    status: 'playable',
    playableStage: 'Stage 2',
    accent: '#d4a24e',
    icon: ICONS.detective,
    defaultSeed: DEFAULT_SEED,
    introduction: {
        kicker: `Case ${CASE_META.caseNumber} · stage 2 flagship simulation`,
        title: 'A deterministic detective case on a real decision kernel'
    },
    blurb: 'Collect evidence, weigh claims against the ledger, and submit real decision requests to the ' +
        'OmniDecide WASM kernel. The scenario logic decides who is guilty; the engine shows which way ' +
        'the lexical evidence points.',
    features: [
        'Nine investigation hours across five museum locations',
        'Ten evidence items with exact wording submitted to the kernel',
        `Five decision primitives exercised: choice, compare, noul, extract, rank`,
        'Transparent engine influence — recommendations, never overrides',
        'Deterministic seed, full reset and byte-identical replay'
    ],
    create: () => createDetectiveModule()
});

registerGame({
    id: 'living-village',
    name: 'Living Village',
    tagline: 'Autonomous villagers with memory, needs and relationships',
    status: 'playable',
    playableStage: 'Stage 3',
    accent: '#5fae6f',
    icon: ICONS.village,
    defaultSeed: VILLAGE_SEED,
    introduction: {
        kicker: 'Seven days · stage 3 village simulation',
        title: 'Steward a village whose daily goals are kernel-arbitrated'
    },
    blurb: 'Steward six villagers over seven days: host gatherings, assign tasks, mediate disputes and ' +
        'share news. Each evening every villager\'s next goal is a real OmniDecide choice request built ' +
        'from their memory and needs; disputes are weighed against the chronicle (compare) and the day ' +
        'is summarized by extract. The reducer applies every effect; the engine advises through rules R1-R3.',
    features: [
        'Six villagers with private memory and a relationship web',
        'Daily goal arbitration via the choice primitive (rule R1)',
        'Dispute mediation via compare against the chronicle (rule R2)',
        'Chronicle digest via extract, display-only (rule R3)',
        'Deterministic day weather, full reset and byte-identical replay'
    ],
    create: () => createLivingVillageModule()
});

registerGame({
    id: 'tactical-command',
    name: 'Tactical Command',
    tagline: 'Squad-level coordination under time pressure',
    status: 'planned',
    accent: '#4f8fc4',
    plannedFor: 'Planned — Stage 3',
    icon: ICONS.tactical,
    blurb: 'A grid-based tactical simulation: squad members, threats with confidence levels, objectives ' +
        'and orders. Every engagement decision is submitted to the kernel with the squad\'s observed ' +
        'evidence, and the command log separates sim outcomes from engine advice.',
    features: [
        'Squad members with roles, loadouts and readiness',
        'Threat assessment via compare/score primitives',
        'Objective prioritization via rank',
        'Order-of-operations constraints owned by scenario logic',
        'After-action review in the Decision Inspector'
    ]
});

registerGame({
    id: 'last-outpost',
    name: 'The Last Outpost',
    tagline: 'Survival resource allocation against a ticking clock',
    status: 'planned',
    accent: '#c47a4f',
    plannedFor: 'Planned — Stage 4',
    icon: ICONS.outpost,
    blurb: 'A frontier outpost with scarce supplies, weather, threats and expeditions. Each cycle the ' +
        'colony evaluates what to build, repair, send or stockpile — evidence-based allocation with ' +
        'the kernel, consequences owned by the simulation.',
    features: [
        'Resource ledger with decay and spoilage',
        'Expedition planning via choice/rank',
        'Threat response via compare',
        'Survival ledger and cycle reports',
        'Deterministic weather seeds'
    ]
});

registerGame({
    id: 'rumors-kingdoms',
    name: 'Rumors & Kingdoms',
    tagline: 'Factions, rumors and the reliability of information',
    status: 'planned',
    accent: '#9a6fc4',
    plannedFor: 'Planned — Stage 4',
    icon: ICONS.kingdoms,
    blurb: 'A political simulation where rumors spread between factions with decay and distortion over ' +
        'time. Time-sensitive evidence and source reliability feed kernel arbitrations on alliances, ' +
        'accusations and war.',
    features: [
        'Rumor propagation with time-decayed reliability',
        'Source credibility model',
        'Alliance and accusation arbitrations via choice/rank',
        'Treaty timeline visualization',
        'Deterministic diplomacy seeds'
    ]
});
