# Handoff / current status

Last updated: 2026-11-11 22:50 UTC

## What is running right now

A build is live on Crave's servers, driven by GitHub Actions. **It does not need
this PC.**

- Workflow run: https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/actions/runs/43105206570
- Commit: `7883e164b20a4bed90f3bf3e01c1951b0229ed3f`
- Job step `Build on Crave` is streaming a Crave job on project **99 (LOS 23.2)**.
- This run replaced #5 (`43101628666`, commit `916dc51`), which was cancelled
  because its token had write scope and Crave prints the command line.

When it finishes, the runner pulls the images back with `crave pull` and the
`publish` job uploads them to a GitHub **release** tagged `aera-selene-<run id>`:

    https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA/releases

Both the pull and the release happen in GitHub Actions, so it survives this PC
being off.

## Next time you sit down

1. Open the run URL above. If it is still `in_progress`, just wait.
2. If it finished:
   - `success` -> download the images from the release tag above.
   - `failure` -> read the log of the `Build on Crave` step. The first real
     compile will most likely surface device-tree errors; fix and push.

## Pipeline as it stands

| Piece | File |
| --- | --- |
| Workflow (runs on ubuntu-latest, only drives the CLI) | `.github/workflows/crave-selene.yml` |
| Remote build script (runs on the Crave node) | `scripts/crave-build.sh` |
| Device tree | `device/xiaomi/selene/` |
| Docs | `docs/BUILDING.md` |

Remote sequence: `repo init` AERA 16 manifest -> `/opt/crave/resync.sh` ->
copy device tree -> `lunch twrp_selene-bp2a-eng` -> `mka adbd bootimage` ->
leave images in `crave-artifacts/` -> runner `crave pull` -> `publish` job
creates the release.

## Token handling

Crave prints every `crave run` command line in its dashboard *Build Commands*
panel, which the whole team can read, so anything on that line is public.

- The build job runs with `permissions: contents: read`. The `GITHUB_TOKEN` it
  passes to the node can therefore only read this one private repo (it needs
  that to clone the device tree) and dies when the job ends.
- Release creation lives in a separate `publish` job that has
  `contents: write` and never leaves GitHub.

## Facts learned the hard way (do not regress)

- Crave's CLI **has no `clone` subcommand**; `crave run` derives the project
  from the `.repo` manifest in the working directory. The workflow therefore
  does a shallow `repo init` of `accupara/los23.2` (`lineage-23.2`) first.
- `crave.conf` **must exist before any crave call**, otherwise every subcommand
  dies with `Caught unknown error: 'Namespace' object has no attribute
  'amsfeaturecap'`. It is written before the CLI is probed.
- Never run `rm -rf *` inside a Crave job - accounts get banned. The script
  wipes only known paths.
- AERA manifest composition: `default.xml` -> `twrp-default.xml` ->
  `aera.xml` + `remove-minimal.xml`. So `vendor/twrp` and `vendor/recovery`
  are both present.
- `selene` is recovery-as-boot (`BOARD_USES_RECOVERY_AS_BOOT := true`), so the
  target is `bootimage`, not `recoveryimage`.

## Secrets

- Repo secrets `CRAVE_USERNAME` and `CRAVE_TOKEN` are set.
- `.gh_token` in this folder is a personal PAT used for the REST pushes.
  **Revoke it when the project is done.**
