# Changelog

All notable changes to CallRecorder are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **Call detection.** CallRecorder notices when Teams, Zoom, Webex, Slack,
  Skype, WhatsApp, Discord, 8x8, MicroSIP, Linphone, RingCentral, or Google
  Meet or Teams in a browser, starts using the microphone. By default it offers
  to record, and to stop when the call ends. **Detect calls** in Settings
  (`call_detection` in `config.json`) can make it record automatically, or turn
  it off.
- Recordings made during a call are named after it, e.g. `Weekly sync (Teams)`
  or `Zoom call 30 Sep 2026 14:02`, instead of `Recording 30 Sep 2026 14:02`.

## [1.2.0] - 2026-09-30

This release adds a Recordings window, a short delay before uploads, and
options for what gets recorded. It also fixes a number of ways a recording or
its settings could be lost, sent to the wrong place or uploaded twice.

**Worth knowing:** CallRecorder records your microphone even when
you're muted in Teams, Zoom or another call app, because their mute doesn't
reach it. Use Pause (`Ctrl+Alt+P`) or Sensitive mode for private moments.

### Added

- **Recordings window** (tray menu > **Recordings...**). Lists every recording
  with its date, length, title, status and details. For the selected recording
  you can open it in Speakr, play it, show it in Explorer, upload it, keep it on
  this PC or delete it.
- **Upload delay.** A recording waits 60 seconds after you stop before it
  uploads. Change it with **Upload after** in Settings, or `upload_delay_seconds`
  in `config.json` (0 to 3600; 0 uploads at once). Sensitive recordings are
  never uploaded, so they have no delay.
- **Keep last recording on this PC** in the tray menu. Stops the last recording
  from uploading, as long as the upload hasn't started.
- **Held recordings.** A recording made before Speakr was set up, or for a
  different server, is never sent on its own. It waits, and the tray menu shows
  **Upload N held recordings...**, which asks whether to upload them or keep
  them on this PC.
- **New `upload_state` values** in each recording's `.json` file:
  - `held`: made before Speakr was set up, or for another server. Waits for you.
  - `missing`: Speakr no longer has it. The audio is kept on this PC.
  - `stuck`: Speakr hadn't finished a day after the upload.
- **Only record audio from these apps** (Settings, or `loopback_apps` in
  `config.json`). Records only the audio the listed apps play, including their
  child processes, so other sounds stay out. Needs a recent Windows 10 or 11.
  If it can't start, CallRecorder warns you and records all playback audio.
- **Separate channels** (Settings, or `separate_channels`). Stereo recording
  with your microphone on the left and everyone else on the right, at 32 kbps.
  Applies from the next recording.
- **Optional echo cancellation** (`echo_cancellation` in `config.json`, off by
  default). Opens the microphone as a communications stream, so Windows can
  apply echo cancellation where the device supports it. Windows may lower other
  apps' sound while recording, which is why it is off by default.
- **Recovered recordings.** If a crash or power cut leaves a recording with no
  readable `.json` file, it is recovered on the next start and kept on this PC
  only. Upload it from the Recordings window if you want it in Speakr.
- **Safer settings file.** If `config.json` can't be read, CallRecorder starts
  in local-only mode and tells you. Saving from Settings keeps the old file as
  `config.json.bad`.
- **Warnings** when Windows blocks desktop apps' access to the microphone, and
  when the microphone has been silent for a minute.
- **http warning.** Settings warns you when the address uses plain `http://` to
  something that isn't on a private network, because your recordings and API
  token would travel unencrypted. It asks before saving one.
- **Tray status** now counts held recordings and recordings with problems.

### Changed

- Testing the connection in Settings runs in the background. Saving only tests
  the connection when you changed the address or token.
- Each recording remembers which server it was made for and is only uploaded
  there. Recordings from before 1.2 belong to the server in Settings.
- Local audio is only deleted for recordings whose notes are ready. Missing,
  stuck, failed, held and local-only recordings keep their audio.
- Pause now cuts at the moment you press it.
- A soft limiter eases loud peaks down instead of hard clipping.
- The tray menu and start and pause notifications only mention keyboard
  shortcuts that registered. If another app uses a shortcut, CallRecorder tells
  you at start-up.
- Warnings and notifications are shown in order of importance, so one no longer
  hides another at start-up.
- Exiting while an upload is running is quicker.
- The `.json` file for each recording also records the server it was made for
  and when its upload is due.

### Fixed

- A crash on a malformed `config.json`, recording `.json` file or Speakr
  response.
- Local audio being deleted when Speakr returned 404 or the server had
  changed. It is now kept, and the recording is marked `missing`.
- Earlier recordings being uploaded to a newly configured server without
  asking.
- Duplicate uploads after an interrupted upload. CallRecorder now checks Speakr
  for the earlier upload first, and says if it had to upload again.
- Recordings being polled forever when Speakr never finished them. They are now
  marked `stuck` after a day.
- Settings freezing, and the hotkeys stopping, while it tested the connection.
- A recording's `.json` file being lost in a power cut.
- Settings not being saved safely. Writes to `config.json` are now atomic.
- An unreadable `config.json` turning local-only mode off.
- About 100 ms of paused audio being recorded on resume.
- Periodic glitches on long calls from clock drift between the microphone and
  the playback device.
- A misleading "No microphone found" when Windows was blocking microphone
  access.
- Warnings overwriting each other.
- "Network error 0" for an invalid Speakr address. It now says the address
  isn't a valid web address.
- Slow exit during an upload.
- Keyboard shortcut hints being shown for shortcuts that failed to register.
- A possible overwrite of an older recording after a clock change.

## [1.1.0] - 2026-09-30

### Added

- Choose the microphone and playback device to record from. The choice takes
  effect straight away, even mid-recording. If the chosen device isn't
  connected, CallRecorder uses the Windows default until it returns.

## [1.0.0] - 2026-09-30

- First release: records both sides of a call to Ogg Opus with one hotkey and
  uploads it to your Speakr server.

[1.2.0]: https://github.com/Feridoun/CallRecorder/releases/tag/v1.2.0
[1.1.0]: https://github.com/Feridoun/CallRecorder/releases/tag/v1.1.0
[1.0.0]: https://github.com/Feridoun/CallRecorder/releases/tag/v1.0.0
