/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * history-instance.js - the GameLab-wide history singleton.
 */

import { HistoryStore } from './history.js';

export const history = new HistoryStore();
