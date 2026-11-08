# Faster X4 Pro development flashes

For an authorized **application-only** update, back up reading state, verify the live
reader/slot and rollback image, write the reviewed app, and independently verify it.
Do not turn this into a full-card audit on every build.

## State backup

After entering USB Drive, identify the external card by its saved volume UUID. Run
with the known Python environment, saving to a new ignored `.local-device/reader-backup/`
directory outside the card:

```sh
python scripts/reader_state_backup.py \
  --volume-uuid "$READER_VOLUME_UUID" \
  --output "$NEW_STATE_BACKUP"
```

Add `--include-web` only when the matching firmware bundle differs from the installed
one and will be replaced. The old bundle then remains available for rollback.

The helper copies settings, complete progress records (including optional anchors),
bookmarks, annotations, reading statistics, pairing and unknown state files. It reads
each selected SD file once while hashing it, then independently hashes the local copy.
It refuses mismatched/internal/unmounted volumes, symlinks, existing backup directories
and destinations on the card. A completed manifest is written only after verification.

It does not traverse or hash books, fonts or dictionaries. It omits known derived
font previews and EPUB parsing/layout caches, leaving unknown state intact. The cache
exclusions follow `lib/Epub/Epub.cpp` (`spineCacheRoot` and the `doc_css` cache) and the
book metadata cache. These files are left on the card, not deleted. This is a scoped
state backup, not a full-card backup. Back up/hash any asset a task actually changes;
a firmware-only update does not require re-reading unchanged library assets.

The owner explicitly accepts dictionary loss during flashing and asks that dictionary
backup/readback not delay development flashes. Preserve dictionaries in place, but
skip their verification. Preserve books, fonts and reading state; this preference is
not permission to delete those files or reset settings.

## Flash transport and checks

Follow the identity, live partition/OTA selection and app-only safeguards in `AGENTS.md`.
Use the exact preserved release image and its matching web bundle. Keep the known-good
application and its checksum; if the live app does not match a known preserved image,
read it back before writing. Safely eject the card before opening the flash tool.

On this X4 Pro/macOS connection, esptool 5.3's helper failed repeatedly while reading
flash around offset `0x3000`; the built-in ROM reader completed the same reads. Use
`--no-stub` for this device instead of repeating failed helper reads or sector retries.
Read/compare bootloader, partition table, OTA metadata and NVS before/after the write;
these total only tens of KiB. Do not skip the independent application verification.

Esptool defaults compression **off** with `--no-stub`. Explicitly enable it for the
ESP32-S3 ROM write. After the preflight has resolved and validated all variables:

```sh
python -m esptool --chip esp32s3 --port "$READER_PORT" \
  --before default-reset --after no-reset --no-stub \
  write-flash --compress --flash-mode keep --flash-freq keep --flash-size keep \
  "$ACTIVE_APP_OFFSET" "$REVIEWED_APP_IMAGE"
```

The active offset must come from the live partition table and OTA metadata, and the
image must fit that live slot. This command is not a substitute for that preflight.
Esptool's ESP32 ROM `flash_defl_begin` supports compressed writes; no firmware changes
are involved. The preserved fastpages2 image compresses from 6,547,248 to 5,047,546
bytes (22.9% less transfer data). Its uncompressed ROM write took 75.1 seconds;
the subsequent 6,550,224-byte USB-fix/text-AA candidate compressed to 5,049,246
bytes and wrote in **59.7 seconds** (2026-11-08), about 20% less write time.
These are separate builds on the same reader, not a controlled benchmark; verification
and reboot add time. Do not reflash merely to benchmark it.

## Finish before optional testing

Verify the running version and crash-free boot, then install/verify a changed web
bundle through the documented card-source API. Compare protected settings/progress;
normal boot/network bookkeeping may change. Do not blindly restore an old snapshot
or overwrite a position the user has advanced. Record exact differences and scope.

Do not leave the reader in USB Drive while running optional library checks. Once the
critical checks finish, safely eject, close serial handles, and tell the user it is
ready to test. If the user disconnects before eject, record that fact without claiming
a successful eject or incomplete checks. Update private installation records and the
known-installed version; keep physical page-turn validation separate from boot success.

Backup selection tests:

```sh
python -m unittest discover -s test/reader_state_backup
```
