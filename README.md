# CallRecorder

A small Windows tray app that records calls and online meetings and sends them
to your self-hosted [Speakr](https://github.com/murtaza-nasir/speakr) server
for transcription and summaries.

It records **both sides of a call** without a virtual audio cable: your
microphone mixed with whatever your speakers or headset are playing (Teams,
Zoom, Google Meet, WhatsApp, a softphone and so on). One hotkey starts it, the
same hotkey stops it, and a notification tells you when Speakr has the notes
ready.

- **Tiny and native.** A single signed `.exe` of about 1 MB, written in C++
  against WASAPI. No runtime, no background services, no window to manage.
- **Crash-proof recordings.** Audio is written to disk as it's recorded (Opus,
  about 11 MB per hour). A crash or power cut loses under half a second, and
  the interrupted recording is still uploaded.
- **Sensitive mode.** One click keeps a recording on this PC only. It's never
  uploaded.
- **Markers.** Press a hotkey at a key moment. In Speakr's notes, each marker
  becomes a link that jumps to that point in the audio.
- **Tags.** Pick Speakr tags from the tray menu before or during a call, so
  Speakr can apply the right summary prompt.
- **Reliable uploads.** Uploads are queued and retried while the server is
  unreachable (laptop offline, VPN down), and the app follows each recording
  until Speakr has finished with it.

## Install

Download from [Releases](https://github.com/Feridoun/CallRecorder/releases/latest):

- **`CallRecorder-Setup-<version>.exe`**: installer. Installs for your user
  only (no admin prompt) and can start CallRecorder when you sign in.
- **`CallRecorder-<version>-portable.zip`**: the same `.exe` with no installer.

Both are code-signed. Needs Windows 10 or 11 (64-bit) and a Speakr server you
can reach.

On first start, CallRecorder asks for:

1. **Your Speakr address**, e.g. `https://speakr.example.com` or
   `http://192.168.1.20:8899`.
2. **An API token.** In Speakr, open *Account settings > API tokens* and create
   one. CallRecorder keeps it in Windows Credential Manager, never in a file.

Click **Test** to check both, then **Save**. You can change them later from the
tray menu under **Speakr connection**.

## Use

A grey dot appears in the tray.

| Action | Shortcut | Tray |
|---|---|---|
| Start / stop recording | `Ctrl+Alt+R` | double-click the icon |
| Pause / resume (paused time is left out) | `Ctrl+Alt+P` | menu |
| Add a marker | `Ctrl+Alt+K` | menu |

Click the icon for the menu, which also has **Sensitive**, **Tags**,
**Microphone** and **Playback** device pickers, upload status, **Upload now**, **Open Speakr**, the recordings folder, settings and
**Start at login**.

Tray icon colours: **grey** idle, **red** recording, **purple** recording in
local-only mode, **amber** paused. Hover over it for the elapsed time and upload
status.

When you stop, the recording uploads straight away. When Speakr has finished
(usually a minute or two), a **Notes ready** notification appears. Click it to
open the recording in Speakr.

### Tags

**Tags** in the tray menu lists your Speakr tags. Ticked tags apply to the
recording in progress and to the ones after it, until you change them. Each
launch starts with the tags in `config.json` ticked (`Call` by default). A tag
listed in `config.json` that doesn't exist in Speakr yet is created on the
first upload.

### Audio devices

By default CallRecorder records Windows' default communications microphone and
default playback device, and follows them when they change. To record a
specific device instead, pick it under **Microphone** or **Playback** in the
tray menu. The choice is saved and takes effect straight away, even
mid-recording. If the chosen device isn't connected, CallRecorder uses the
Windows default and switches back when the device returns.

### Sensitive recordings

Where the audio ends up depends on how your Speakr server is set up. It might
transcribe locally (e.g. WhisperX) or send audio to a cloud speech or
summarisation service. For anything that must stay on your PC, tick
**Sensitive: keep on this PC only** before or during the recording. The tray
dot turns purple and the recording is never uploaded. Sensitive mode stays on
for the next recordings until you untick it.

## Please record lawfully

The rules on recording calls vary by country and state. Many places require
everyone on the call to consent, and some workplaces have their own policies.
Tell people you're recording. CallRecorder doesn't announce itself to other
people on the call.

## Settings

Tray menu > **Settings** opens `%APPDATA%\CallRecorder\config.json`. Changes
apply within 10 minutes, or straight away after **Upload now**.

```json
{
  "server_url": "https://speakr.example.com",
  "tags": ["Call"],
  "hotwords": "",
  "keep_audio_days": 14,
  "sensitive_by_default": false,
  "microphone": "",
  "speakers": ""
}
```

| Key | Meaning |
|---|---|
| `server_url` | Your Speakr address. It's easier to set it through **Speakr connection** |
| `tags` | Tags ticked when CallRecorder starts |
| `hotwords` | Comma-separated names and jargon to help transcription, e.g. `"Anika, Kubernetes, SLA"` |
| `keep_audio_days` | Delete local audio this many days after Speakr has finished with it (0 = keep forever). Local-only and failed recordings are never deleted |
| `sensitive_by_default` | Start every launch in local-only mode |
| `microphone`, `speakers` | Windows device IDs to record; empty follows the Windows default. Set them from the tray menu |

## Where recordings go

`%LOCALAPPDATA%\CallRecorder\sessions\` (tray menu > **Open recordings
folder**). This is kept out of Documents because Documents is often synced to
OneDrive.

Each recording is `<date>_<time>.opus` (48 kHz mono, plays in VLC, browsers and
most players) plus a `.json` file whose `upload_state` shows its progress:

| `upload_state` | Meaning |
|---|---|
| `local_only` | Sensitive; never uploaded |
| `pending` | Waiting to upload (retried while Speakr is unreachable) |
| `uploaded` | In Speakr, being transcribed and summarised |
| `done` | Notes ready |
| `failed` | Speakr couldn't process it (see `upload_error`; reprocess it in Speakr) |
| `rejected` | Speakr refused the upload (see `upload_error`; tray menu > **Retry refused uploads**) |

Uninstalling leaves your recordings and settings in place.

## Good to know

- Unless you pick one, the microphone is Windows' **default communications
  device**, which Teams and Zoom normally use too. If you plug in a headset or
  connect Bluetooth mid-call, recording moves to the new device.
- Without a headset, your microphone also picks up the other side from your
  speakers, so their voices appear twice, slightly offset. Use a headset for
  clean transcripts.
- A single click opens the menu after a short pause (the double-click time), so
  a double-click can start or stop recording instead.
- If Speakr sits behind a proxy or tunnel with an upload size limit (Cloudflare
  allows 100 MB, about 9 hours of audio), very long recordings will be refused.

## Build from source

Needs Visual Studio 2022 with the *Desktop development with C++* workload
(includes CMake, Ninja and vcpkg).

```powershell
.\build.ps1                  # or: .\build.ps1 -Config debug
```

The output is `build\release\CallRecorder.exe`. Dependencies (`opus`,
`libopusenc`, `nlohmann-json`) come from vcpkg in manifest mode and are linked
statically.

To build the installer and portable zip as well (needs
[Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```powershell
.\tools\release.ps1 -AllowUnsigned           # test build
.\tools\release.ps1 -CertThumbprint <sha1>   # signed release (see tools\sign.ps1)
```

| File | Role |
|---|---|
| `src/main.cpp` | Tray icon, menu, hotkeys, tags, session lifecycle |
| `src/Recorder.*` | WASAPI microphone + loopback capture, mixing, device selection and changes |
| `src/OpusFileWriter.*` | Crash-tolerant Ogg Opus output |
| `src/Session.*` | The `.json` file for each recording, and crash recovery |
| `src/Uploader.*` | Background upload queue, Speakr status polling, tag lookup, audio retention |
| `src/ConnectionDialog.*` | The Speakr connection dialog |
| `src/HttpClient.*` | WinHTTP client (streams uploads from disk) |
| `src/Config.*` | `config.json` and the Credential Manager token |
| `installer/CallRecorder.iss` | Inno Setup script |

## Roadmap ideas

- Detect meetings (Teams, Zoom, etc. start using the microphone) and offer to record
- Prompt for a title when you stop
- Separate channels for you and the other side, for better speaker labels
- Configurable hotkeys

Issues and pull requests are welcome.

## Licence

[MIT](LICENSE). CallRecorder is an independent project, not affiliated with
Speakr. Third-party licences are in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
