# Changelog

All notable changes to this project will be documented in this file.

## [1.0.0] - 2026-11-03

- feat(library): group screenshots and recordings by day, month or year
- chore: satisfy clang-tidy 23
- chore(recorder): log audio re-syncs
- fix: VA-API recordings no longer drop frames at the start
- fix: drop fewer frames when a recording starts
- perf: open region selection faster
- fix: don't touch the autostart entry from a build tree
- fix: quitting during a recording finishes the file
- fix: restrict an existing config file before writing the S3 secret
- fix: reject invalid images from KWin
- refactor: share recorder names, limits and commands via recorderprotocol.h
- refactor: compare settings with a defaulted operator==
- chore(recorder): log ignored encoder options
- fix: free the CUDA context when NVENC isn't used
- fix(recorder): fail fast when the event descriptor can't be created
- fix: correct OpenH264 bitrate at high resolutions
- fix: support 64-bit PipeWire serials
- fix: validate save folders in settings
- build: drop unused libswresample
- build: require Qt 6.10, LayerShellQt 6.6 and PipeWire 1.0
- fix: make recordings of still screens seekable
- fix: show only an error when a screenshot can't be saved
- fix: stopping before the first frame no longer reports a failure
- fix: same-second screenshots no longer overwrite each other
- fix: delete the recording when it can't be finalized
- fix: don't follow redirects when uploading to S3
- fix(release): handle any changelog header length
- feat(release): add preminor and premajor bumps
- fix(release): refuse prerelease bumps that would downgrade
- fix(release): include prerelease changes in stable release notes
- chore: ignore build-gui and build-rec
- feat: add library
- fix(packaging): rename the package to blink-capture
- feat: configurable folder for saving captures
- feat: name window captures after their app
- fix: update upload path documentation and correct build command in PKGBUILD
- feat(s3): allow customizing upload path
- first commit

## [0.1.1-alpha.2] - 2026-10-15

- feat: add library
- fix(packaging): rename the package to blink-capture
- feat: configurable folder for saving captures
- feat: name window captures after their app
- fix: update upload path documentation and correct build command in PKGBUILD
- feat(s3): allow customizing upload path

## [0.1.1-alpha.1] - 2026-10-14

- first commit

