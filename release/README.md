# HoloFan firmware release

Version 0.9.19, built from commit 15713a9 with firmware/holofan/build.sh (ESP-IDF 11edebdbbe, release/v5.4).
Not yet run on hardware (manifest.json: tested_on_hardware false).

    manifest.json    version, commit, and each file's name, size and SHA-256
    holofan.bin      the application image: the one to send over Wi-Fi

manifest.json is the file for a program to read: it names the OTA image
(files.ota.file) and gives its size and SHA-256 to check before sending.

## Updating over Wi-Fi

Keep a WCB switched on and in range while you update: the new image must receive a mesh frame within 30 s of booting, or the node rolls back to the previous image.

Put the node in maintenance mode, then:

    curl -X POST -H 'Content-Type: application/octet-stream' \
         --data-binary @release/holofan.bin http://holofan.local/api/ota

docs/MAINTENANCE_API.md has the whole procedure and the rollback rule.

## First wired flash

Not from this folder: a stock fan needs its backup checked first. Build in
the repository and use firmware/holofan/flash_first.sh.

## Making a release

Build on a clean checkout, copy the images here under the same names,
and update manifest.json (version, commit, sizes, SHA-256) in the same
commit. The binaries hold the build machine's paths as text, so build in a
directory whose path says nothing private, and run
`python3 tools/check_private.py` before committing.
