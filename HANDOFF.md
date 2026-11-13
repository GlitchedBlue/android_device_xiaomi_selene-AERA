# Handoff / current status

Last updated: 2026-11-13

## What happened

Crave.io shut down its FOSS build service in November 2026. The workflow that
drove it (`crave-selene.yml` + `scripts/crave-build.sh`) has been deleted; run
#6 sat queued all night and was cancelled by the 6-hour job timeout without ever
starting.

The build now runs **directly on a GitHub-hosted runner**, using the same recipe
the other AERA device ports use.

## What is running right now

- Workflow: `.github/workflows/build-aera.yml`
- Run: https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/actions
- Repo is **public** (required — see below)
- Expect **2-2.5 hours** per attempt

On success the images are published to a GitHub release tagged
`aera-selene-<run id>`:

    https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/releases

## Next time you sit down

1. Open the Actions tab. If a run is `in_progress`, wait.
2. `success` -> download the image from the release tag.
3. `failure` -> read the failing step. The build is retried as-is with
   "Re-run failed jobs" before changing anything; these builds fail
   intermittently on the runner.

## Pipeline as it stands

| Piece | File |
| --- | --- |
| Workflow (compiles on the runner) | `.github/workflows/build-aera.yml` |
| Device tree | `device/xiaomi/selene/` |
| Docs | `docs/BUILDING.md` |

Sequence: `maximize-build-space` (pools disks into a ~60-115 GB volume) ->
install deps + `repo` -> `repo init --depth=1` AERA 16 -> `repo sync` with
retries -> copy device tree -> `rm -rf .repo` -> `export AERA_BUILD_DEVICE=selene`
-> `source build/envsetup.sh` -> `lunch twrp_selene-bp2a-eng` ->
`mka adbd bootimage` (falls back to `recoveryimage`) -> release upload.

## Facts learned the hard way (do not regress)

- **The repository must stay public.** A private repo gets a 2 vCPU / 8 GB
  runner and Soong is OOM-killed ~48 minutes into the build. The AERA port
  `HIZVOICE/...camellia-AERA` is private and went 0-for-16; every port that
  produces images is public. Public also means unlimited Actions minutes.
- **`maximize-build-space` is mandatory.** Without it the runner has ~14 GB free
  and the sync dies with `ENOSPC`. Do not shrink `root-reserve-mb` /
  `temp-reserve-mb` — the action itself then fails with
  `fallocate: invalid length value specified`.
- **`rm -rf .repo` before building** reclaims ~20 GB. `.repo` holds the history
  of ~1000 projects and is useless once the working tree exists.
- **`AERA_BUILD_DEVICE` must be exported before `source build/envsetup.sh`**
  (AERA wiki). Setting it inside the vendorsetup guard is too late and lets
  another tree's exports leak in.
- The AERA manifest is ~989 projects; `remove-minimal.xml` removes 601 of them.
  Composition: `default.xml` -> `twrp-default.xml` -> `aera.xml` +
  `remove-minimal.xml`, so `vendor/twrp` and `vendor/recovery` are both present.
- `selene` is recovery-as-boot (`BOARD_USES_RECOVERY_AS_BOOT := true`), so the
  target is `bootimage`, not `recoveryimage`.
- The products keep the `twrp_` prefix in the lunch name; that is not the UI
  branding (AERA wiki).

## Open questions

- `aera_selene.mk` sets `AERA_ENABLE_WLAN := 0` to disable WLAN. The wiki warns
  that some AERA switches treat any non-empty value — including `0` — as
  enabled. The consumer is `bootable/recovery/aera_config.mk`, which has not been
  read yet. Check it before trusting that switch.

## Secrets

- `CRAVE_USERNAME` / `CRAVE_TOKEN` are now unused; they can be deleted.
- `.gh_token` in this folder is a personal PAT used for the REST pushes.
  **Revoke it when the project is done.**
