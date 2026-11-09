<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Test fixtures

Model files used by `tests/smoke_test.c` (all small binary files; see
[docs/MODEL.md](../../docs/MODEL.md) for the format).

| File | Content | Expected |
|---|---|---|
| `default_model.odkmodel` | the built-in default constants as a v1 file | loads; every output identical to no model |
| `fs1_zero_extras.odkmodel` | v2, default constants, six extra weights all 0 | loads |
| `fs1_number_only.odkmodel` | v2, default constants, only the number-match weight (0.5) | loads |
| `corrupt_bad_magic.odkmodel` | v1 with magic `ODKMODLX` | rejected |
| `corrupt_version.odkmodel` | v1 magic with `format_version = 2` | rejected |
| `corrupt_nan.odkmodel` | v1 with a NaN weight, valid hash | rejected |
| `corrupt_hash.odkmodel` | v1 with a flipped hash byte | rejected |
| `corrupt_truncated.odkmodel` | first 80 bytes of a v1 file | rejected |
| `corrupt_oversized.odkmodel` | a v1 file plus 100 extra bytes | rejected |
| `corrupt_random.odkmodel` | 128 pseudo-random bytes | rejected |
| `corrupt_negscale.odkmodel` | v1 with `logit_scale = -6`, valid hash | rejected |
| `corrupt_v2_reserved.odkmodel` | v2 with a nonzero reserved byte at offset 92, valid hash | rejected |

The valid fixtures can be recreated with `build/model_pack`
(`make build/model_pack`); the corrupt ones are the listed one-line mutations
of a default file.
