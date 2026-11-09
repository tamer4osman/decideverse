// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tamer Osman
//
// Playwright E2E configuration for the OmniDecide v2.0.0 sidecar.
// Two servers are started before the tests:
//   - the sidecar in HTTP mode with the synthetic example fixtures
//     registered through the F7 --data path (zero code changes)
//   - a tiny static server for the browser test page

const { defineConfig } = require('@playwright/test');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const SIDECAR_PORT = 18320;
const PAGE_PORT = 18321;

module.exports = defineConfig({
  testDir: './specs',
  timeout: 30000,
  use: {
    baseURL: `http://127.0.0.1:${SIDECAR_PORT}`,
    headless: true,
  },
  webServer: [
    {
      command: './build/odk_sidecar --http ' + SIDECAR_PORT +
               ' --data examples/synthetic/data',
      url: `http://127.0.0.1:${SIDECAR_PORT}/health`,
      cwd: ROOT,
      reuseExistingServer: false,
      timeout: 15000,
    },
    {
      command: 'node server.js ' + PAGE_PORT,
      url: `http://127.0.0.1:${PAGE_PORT}/`,
      cwd: __dirname,
      reuseExistingServer: false,
      timeout: 15000,
    },
  ],
});
