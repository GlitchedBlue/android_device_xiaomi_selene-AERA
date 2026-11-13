# Handoff / current status

Last updated: 2026-11-13

## Where this stands

The build runs **directly on a GitHub-hosted runner**, using the recipe the
other AERA device ports use (AzzyC's `Action-OFRP-Builder`, 14/20 successes at
78-156 min on the same manifest).

- Workflow: `.github/workflows/build-aera.yml`
- Actions: https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/actions
- Repo is **public** (required — see below)
- Expect **~1.5-2.5 hours** per attempt
- Source sync alone is ~20 min and does not change between attempts, so a
  config error costs a full round trip. That is why the whole board/product
  config was audited against the reference index, not just fixed one error at
  a time.

On success the images are published to a GitHub release tagged
`aera-selene-<run id>`:

    https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/releases

## Run history

| Run | Result | Cause |
| --- | --- | --- |
| #1 (`43495414023`) | failure, 25 min | `PRODUCT_USE_DYNAMIC_PARTITIONS` set in `BoardConfig.mk` -> `cannot assign to readonly variable` |

Run #1 got all the way through `Maximize build space`, deps and a clean
76 GB sync. It died in 20 seconds at `lunch`, which is a *good* failure: the
infrastructure works, the tree was wrong.

## Next time you sit down

1. Open the Actions tab. If a run is `in_progress`, wait.
2. `success` -> download the image from the release tag.
3. `failure` -> read the failing step. Retry as-is with "Re-run failed jobs"
   before changing anything; these builds fail intermittently on the runner.
   But read the log first: a config/make error reproduces every time.

## Pipeline as it stands

| Piece | File |
| --- | --- |
| Workflow (compiles on the runner) | `.github/workflows/build-aera.yml` |
| Device tree | `device/xiaomi/selene/` |
| Docs | `docs/BUILDING.md` |

Sequence: `slimhub` cleanup -> 30 GB swap -> deps + `repo` ->
`repo init --depth=1` AERA 16 -> `repo sync` with retries -> copy device tree ->
`rm -rf .repo` -> `export AERA_BUILD_DEVICE=selene` ->
`source build/envsetup.sh` -> `lunch twrp_selene-bp2a-eng` ->
`mka adbd bootimage` (falls back to `recoveryimage`) -> release upload.

## Facts learned the hard way (do not regress)

- **The repository must stay public.** A private repo gets a 2 vCPU / 8 GB
  runner and Soong is OOM-killed ~48 minutes into the build. The AERA port
  `HIZVOICE/...camellia-AERA` is private and went 0-for-16; every port that
  produces images is public. Public also means unlimited Actions minutes.
- **Run on `ubuntu-24.04`, and do NOT use `maximize-build-space`.** The 24.04
  public image has a **145 GB** root disk (measured from a successful AzzyC
  run). On 22.04, `easimon/maximize-build-space` pooled us down to **84 GB** —
  it was actively costing space, and it failed with `fallocate: invalid length
  value specified` if the reserves were lowered.
- **30 GB of swap** (`pierotofy/set-swap-space`) is what lets `-j$(nproc)` on 4
  cores survive Soong's peak RSS in 16 GB of RAM.
- **`rm -rf .repo` before building** reclaims ~18 GB. `.repo` holds the history
  of ~1000 projects and is useless once the working tree exists.
- **`AERA_BUILD_DEVICE` must be exported before `source build/envsetup.sh`**
  (AERA wiki). Setting it inside the vendorsetup guard is too late and lets
  another tree's exports leak in.
- **Product variables are readonly in `BoardConfig.mk`.** `product_config.mk`
  runs before `board_config.mk` and calls `readonly-product-vars`, so anything
  `PRODUCT_*` assigned in a board config is a hard make error. It goes in the
  product make file (`device.mk`).
- The AERA manifest is ~989 projects; `remove-minimal.xml` removes 601 of them.
  Composition: `default.xml` -> `twrp-default.xml` -> `aera.xml` +
  `remove-minimal.xml`, so `vendor/twrp` and `vendor/recovery` are both present.
- `selene` is recovery-as-boot (`BOARD_USES_RECOVERY_AS_BOOT := true`), so the
  target is `bootimage`, not `recoveryimage`.
- The products keep the `twrp_` prefix in the lunch name; that is not the UI
  branding (AERA wiki). The combo is `twrp_selene-bp2a-eng` — there is no
  `aera_selene` product and `twrp_selene-eng` is not a valid combo.

## Config audit against the AERA reference (2026-10-15, 149 flags)

Every `AERA_*` in the tree was checked for placement and for a real consumer in
`aera_config.mk` / `aera_build.mk` / `AERA_A16.sh`. Fixes applied:

| Flag | Was | Now |
| --- | --- | --- |
| `PRODUCT_USE_DYNAMIC_PARTITIONS` | `BoardConfig.mk` (hard error) | `device.mk` only |
| `AERA_SCREEN_H` / `AERA_STATUS_H` | `aera_selene.mk` | `BoardConfig.mk` |
| `AERA_MAINTAINER` / `AERA_NO_TREBLE_COMPATIBILITY_CHECK` | `aera_selene.mk` | `vendorsetup.sh`, inside the device guard |
| `AERA_USE_FSCRYPT_POLICY` | `1` (= v1) | `2` (= v2, matches our `:v2` fstab) |
| `AERA_DELETE_AROMAFM` / `AERA_DELETE_MAGISK_ADDON` | exported | removed — dead in A16, nothing reads them |

Two flags the reference page does **not** list are nevertheless live in the A16
backend, so they were kept:

- `AERA_OPTIONS_LIST_NUM` -> `aera_config.mk:46` -> `aera_build.mk:724`
- `AERA_WORKAROUND_BACKUP_BUG` -> `aera_config.mk:58` -> `aera_build.mk:819`
  (the backend already defaults this to 1 when unset)

## Checked: the WLAN switch is correct

`aera_selene.mk` sets `AERA_ENABLE_WLAN := 0` to disable WLAN. The wiki warns
that some switches treat any non-empty value as enabled, so this was worth
verifying rather than trusting.

Traced it end to end:

- `bootable/recovery/aera_config.mk` maps it verbatim:
  `aera-map-config,ENABLE_WLAN,OF_ENABLE_WLAN` -> `OF_ENABLE_WLAN := $(AERA_ENABLE_WLAN)`
- `bootable/recovery/aera_defaults.go` compares against the literal `"1"`:

  ```go
  if getMakeVars(ctx, "OF_ENABLE_WLAN") == "1" {
      aeraFlags = append(aeraFlags, "-DOF_ENABLE_WLAN")
  }
  ```

So `"0"` != `"1"` and WLAN stays off, exactly as intended. No change needed.

Note the contrast with `AERA_USE_FSCRYPT_POLICY`, where `1` *does* mean v1 and
everything else means v2. The semantics are per-flag; check the consumer.

## Secrets

- `CRAVE_USERNAME` / `CRAVE_TOKEN` are now unused; they can be deleted.
- `.gh_token` in this folder is a personal PAT used for the REST pushes.
  **Revoke it when the project is done.**
