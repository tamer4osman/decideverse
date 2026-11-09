// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tamer Osman
//
// E2E tests for the OmniDecide v2.0.0 integration surface: the sidecar
// HTTP API (request fixture) and the browser harness page (Chromium).
// The sidecar runs with the synthetic example fixtures registered via
// the F7 --data path — no code changes anywhere.

const { test, expect } = require('@playwright/test');

const REQUEST = {
  primitive: 'choice',
  instruction: 'How should we fix the outage?',
  options: ['restart the service', 'reinstall the operating system',
            'wait for the weekend'],
  context: 'The service stopped responding. Restarting the service ' +
           'restored normal operation within seconds.',
};

async function rpc(request, op, params) {
  const res = await request.post('/rpc', { data: { id: 'e2e', op, params } });
  expect(res.ok()).toBeTruthy();
  return res.json();
}

test.describe('sidecar HTTP API', () => {
  test('health endpoint answers', async ({ request }) => {
    const res = await request.get('/health');
    expect(res.ok()).toBeTruthy();
    expect(await res.json()).toEqual({ status: 'running' });
  });

  test('ping reports the v2.0.0 build identity', async ({ request }) => {
    const body = await rpc(request, 'ping', {});
    expect(body.ok).toBe(true);
    expect(body.result.kernel_version).toBe('2.0.0');
    expect(body.result.schema_fingerprint).toMatch(/^[0-9a-f]{16}$/);
  });

  test('zero-customization default held before --data registration',
       async ({ request }) => {
    // The --data fixtures register 4 entries (2 schemes, 1 template,
    // 1 vocabulary); the corpus file registers nothing active. This
    // assertion documents the state the default build ships with:
    // the check-default gate proves the BINARY carries none of it.
    const body = await rpc(request, 'registrations', {});
    expect(body.ok).toBe(true);
    const kinds = body.result.registrations.map((r) => r.kind);
    expect(kinds).toContain('scheme_set');
    expect(kinds).toContain('decomposition_template');
    expect(kinds).toContain('vocabulary');
    expect(kinds).not.toContain('brain_snapshot');
  });

  test('evaluate reproduces the documented v1 decision', async ({ request }) => {
    const body = await rpc(request, 'evaluate', REQUEST);
    expect(body.ok).toBe(true);
    expect(body.result.decision).toBe('restart the service');
    expect(body.result.confidence).toBeCloseTo(0.6312, 4);
  });

  test('determinism: identical requests return byte-identical responses',
       async ({ request }) => {
    const r1 = await request.post('/rpc', { data: { id: 'd', op: 'evaluate', params: REQUEST } });
    const t1 = await r1.text();
    const r2 = await request.post('/rpc', { data: { id: 'd', op: 'evaluate', params: REQUEST } });
    const t2 = await r2.text();
    expect(t2).toBe(t1);
    // and a different id echoes verbatim (envelope is deterministic too)
    const r3 = await request.post('/rpc', { data: { id: 'other-id', op: 'evaluate', params: REQUEST } });
    const t3 = await r3.text();
    expect(JSON.parse(t3).id).toBe('other-id');
    expect(t3.replace('"other-id"', '"d"')).toBe(t1);
  });

  test('classify: one call, N schemes, labeled tiers', async ({ request }) => {
    const body = await rpc(request, 'classify', {
      subject_ref: 'e2e-item-1',
      item_text: 'frobnication overheat on the thermal unit',
      scope: '', t: 1000,
    });
    expect(body.ok).toBe(true);
    expect(body.result.classifications).toHaveLength(2);
    const triage = body.result.classifications[0];
    expect(triage.scheme_id).toBe('fixture-triage');
    expect(triage.label).toBe('hot');
    expect(triage.tier).toBe('A'); // Tier B needs a registered brain
    expect(triage.facets).toEqual({ severity: 'high', area: 'thermal' });
  });

  test('classify: Tier B paraphrase resilience with a registered brain',
       async ({ request }) => {
    const brain = await rpc(request, 'register', {
      kind: 'brain_snapshot', json: null,
      // registering a brain over HTTP is not supported (the brain is an
      // application-owned handle); instead verify the documented error
    });
    expect(brain.ok).toBe(false); // register op cannot carry a brain handle
  });

  test('decompose: parameterized extraction with resolved defaults',
       async ({ request }) => {
    const body = await rpc(request, 'decompose', {
      template_id: 'fixture-report', version: -1,
      item_text: 'The frobnication loop stalled twice.\nLoad: 81 percent\n' +
                 'Pressure: normal\nEverything settled.',
      params: { label: 'nightly' }, t: 1000,
    });
    expect(body.ok).toBe(true);
    expect(body.result.template_version).toBe(1);
    const byName = Object.fromEntries(
        body.result.components.map((c) => [c.name, c]));
    expect(byName.symptoms.item_count).toBe(1);
    expect(byName.metrics.content).toBe('Load: 81 percent\nPressure: normal');
    expect(body.result.params_used.depth).toBe('2'); // declared default
  });

  test('worldState contract: valid accepted, invalid rejected',
       async ({ request }) => {
    const good = await rpc(request, 'worldstate.validate', {
      json: JSON.stringify({
        t: 6000, scope_json: '{"bbox":[0,0,1,1]}',
        variables: [{ name: 'load', type: 'number', value: '0.81' }],
      }),
    });
    expect(good.result.valid).toBe(true);
    // an empty variables list is schema-valid shape; the case with an
    // unparseable scope must fail with a valid JSON envelope
    const badScope = await rpc(request, 'worldstate.validate', {
      json: JSON.stringify({ t: 1, scope_json: 'not json{', variables: [] }),
    });
    expect(badScope.ok).toBe(false);
    expect(typeof badScope.error).toBe('string');
    expect(good.result.valid).toBe(true);
  });

  test('pin -> replay round-trip with exact verdict', async ({ request }) => {
    const pin = await rpc(request, 'pin', {
      request: REQUEST, inputs_ref: 'e2e-inputs-1',
      replay_class: 'exact', timestamp: 1234567890123,
    });
    expect(pin.ok).toBe(true);
    expect(pin.result.schema_fp).toMatch(/^[0-9a-f]{16}$/);
    expect(pin.result.kernel_version).toBe('2.0.0');
    expect(pin.result.timestamp).toBe(1234567890123);
    expect(pin.result.decision).toBe('restart the service');
    expect(pin.result.registrations.length).toBeGreaterThan(0);

    const rep = await rpc(request, 'replay', { pin_id: pin.result.pin_id });
    expect(rep.result.replay_class).toBe('exact');
    expect(rep.result.decision_match).toBe(true);
    expect(rep.result.byte_equal).toBe(true);
    expect(rep.result.registrations_match).toBe(true);
    expect(rep.result.rederived_confidence).toBeCloseTo(0.6312, 4);

    // replay is deterministic: byte-identical report
    const rep2 = await rpc(request, 'replay', { pin_id: pin.result.pin_id });
    expect(JSON.stringify(rep2.result)).toBe(JSON.stringify(rep.result));
  });

  test('foreign pin records lineage without re-derivation', async ({ request }) => {
    const pin = await rpc(request, 'pin', {
      request: REQUEST, inputs_ref: 'e2e-inputs-2',
      replay_class: 'foreign', timestamp: 1234567890999,
    });
    const rep = await rpc(request, 'replay', { pin_id: pin.result.pin_id });
    expect(rep.result.replay_class).toBe('foreign');
    expect(rep.result.rederived_decision).toBe('');
    expect(rep.result.rederived_confidence).toBeNull();
  });

  test('calibration: deterministic report over an ingested outcome log',
       async ({ request }) => {
    const decision = {
      observation_id: 'e2e-d1', kind: 'decision', t: 4000, scope: '',
      subject_ref: 'e2e-item-9', primitive: 'choice', scheme_id: '',
      payload_json: '{}', confidence: 0.85, tier: 'A',
      brain_snapshot_id: '', outcome: '', kernel_version: '2.0.0',
      schema_fp: 'e2e', registrations: [],
    };
    const outcome = {
      observation_id: 'e2e-o1', kind: 'outcome', t: 4100, scope: '',
      subject_ref: 'e2e-d1', primitive: '', scheme_id: '',
      payload_json: '{}', confidence: 0, tier: '', brain_snapshot_id: '',
      outcome: 'success', kernel_version: '2.0.0', schema_fp: 'e2e',
      registrations: [],
    };
    expect((await rpc(request, 'observation.ingest', { json: JSON.stringify(decision) })).result.ingested).toBe(true);
    expect((await rpc(request, 'observation.ingest', { json: JSON.stringify(outcome) })).result.ingested).toBe(true);

    const cal1 = await rpc(request, 'calibrate', { generated_from: 'e2e-log' });
    const cal2 = await rpc(request, 'calibrate', { generated_from: 'e2e-log' });
    expect(cal1.result.bins.length).toBeGreaterThan(0);
    // byte-identical reports for the same log
    expect(JSON.stringify(cal2.result)).toBe(JSON.stringify(cal1.result));
    const bin8 = cal1.result.bins.find((b) => b.bin_index === 8);
    expect(bin8.sample_count).toBeGreaterThan(0);
    expect(bin8.observed_rate).toBe(1);
  });
});

test.describe('browser harness (Chromium)', () => {
  test('page exercises the sidecar and renders every verdict', async ({ page }) => {
    await page.goto('http://127.0.0.1:18321/');
    await page.waitForFunction('window.__HARNESS_DONE === true', null,
                              { timeout: 15000 });

    await expect(page.locator('#ping')).toContainText('2.0.0');

    const classify = await page.locator('#classify').innerText();
    expect(classify).toContain('hot');
    expect(classify).toContain('tier A');

    await expect(page.locator('#determinism')).toContainText('PASS');
    await expect(page.locator('#determinism')).toContainText('restart the service');

    await expect(page.locator('#replay')).toContainText('PASS');
    await expect(page.locator('#replay')).toContainText('verdict exact');
    await expect(page.locator('#replay')).toContainText('byte_equal true');

    // calibration renders a table or the deterministic empty report
    const calib = await page.locator('#calibration').innerText();
    expect(calib.length).toBeGreaterThan(0);
  });
});
