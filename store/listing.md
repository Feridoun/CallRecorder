# Microsoft Store submission

Everything Partner Center asks for, ready to paste. CallRecorder is submitted
as an **EXE app**: the Store downloads the signed Inno Setup installer from
GitHub Releases and runs it silently, so no MSIX packaging is needed.

## Before each submission

1. Bump the version in `CMakeLists.txt` and `res/CallRecorder.manifest`, then
   build and sign with `.\tools\release.ps1 -CertThumbprint <sha1>`.
2. Publish a GitHub release tagged `v<version>` with
   `CallRecorder-Setup-<version>.exe` attached. The Store needs a URL that
   always serves the same file, so never replace an asset in an existing
   release; publish a new version instead.
3. In Partner Center, open the app > **Packages**, and set the package URL to
   `https://github.com/Feridoun/CallRecorder/releases/download/v<version>/CallRecorder-Setup-<version>.exe`
4. Update **What's new** in the listing, then submit.

## Packages

| Field | Value |
|---|---|
| Package URL | `https://github.com/Feridoun/CallRecorder/releases/download/v1.1.0/CallRecorder-Setup-1.1.0.exe` |
| Architecture | x64 |
| Language | English (United States) |
| App type | EXE |
| Installer parameters (silent) | `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /MERGETASKS="!startup"` |

`/MERGETASKS="!startup"` stops a Store install from turning on **Start at
login** without asking. Users can turn it on later from the tray menu.

Inno Setup exit codes:

| Code | Scenario |
|---|---|
| 0 | Installation successful |
| 2 | Installation cancelled by user (before installing) |
| 5 | Installation cancelled by user (during installing) |
| 7 | CallRecorder is running and didn't exit when asked. Exit it from the tray and try again (custom) |
| 8 | Reboot required |
| 1, 3, 4 | Setup failed (miscellaneous) |

## Store listing (English)

**Product name:** CallRecorder
If the name is taken, try `CallRecorder for Speakr` or `Speakr Call Recorder`.

**Short description** (1,000 characters max):

> Record both sides of any call or online meeting (Teams, Zoom, Google Meet, WhatsApp, a softphone) with one hotkey, and get a transcript and summary from your own Speakr server.

**Description** (10,000 characters max):

> CallRecorder is a small tray app that records calls and online meetings and sends them to your self-hosted Speakr server for transcription and summaries.
>
> It records both sides of a call without a virtual audio cable: your microphone, mixed with whatever your speakers or headset are playing. That works with Teams, Zoom, Google Meet, WhatsApp, softphones and any other calling app. Press Ctrl+Alt+R to start and again to stop. A notification tells you when Speakr has the notes ready.
>
> TINY AND NATIVE
> A single signed app of about 1 MB, written in C++. No runtime, no background services, no window to manage.
>
> CRASH-PROOF RECORDINGS
> Audio is written to disk as it's recorded, in Opus format (about 11 MB per hour). A crash or power cut loses under half a second, and the interrupted recording is still uploaded.
>
> SENSITIVE MODE
> One click keeps a recording on this PC only. It's never uploaded.
>
> MARKERS AND TAGS
> Press Ctrl+Alt+K at a key moment. In Speakr's notes, each marker links to that point in the audio. Pick Speakr tags from the tray menu so Speakr applies the right summary prompt.
>
> RELIABLE UPLOADS
> Uploads are queued and retried while your server is unreachable (for example when you're offline or the VPN is down).
>
> PRIVATE BY DESIGN
> No accounts, analytics or telemetry. CallRecorder only talks to the Speakr server you choose, and keeps your API token in Windows Credential Manager.
>
> REQUIREMENTS
> A Speakr server you can reach (https://github.com/murtaza-nasir/speakr). Without one, CallRecorder still records to your PC.
>
> PLEASE RECORD LAWFULLY
> The rules on recording calls vary by country and state, and many places require everyone on the call to consent. Tell people you're recording.
>
> CallRecorder is free and open source (MIT licence) and isn't affiliated with Speakr.

**What's new in this version:**

> Choose which microphone and playback device to record from the tray menu. The choice applies straight away, even mid-recording.

**Product features** (up to 20, 200 characters each):

1. Records both sides of any call: your microphone plus your PC's playback audio
2. One hotkey to start and stop (Ctrl+Alt+R), plus pause and markers
3. Uploads to your self-hosted Speakr server for transcripts and summaries
4. Sensitive mode keeps a recording on this PC only
5. Crash-proof: audio is saved as it's recorded
6. Choose the microphone and playback device, or follow the Windows defaults
7. Uploads queued and retried while offline
8. No accounts, analytics or telemetry

**Search terms** (7 max):
`call recorder`, `meeting recorder`, `teams recorder`, `zoom recorder`, `speakr`, `transcription`, `voice recorder`

**Category:** Productivity (subcategory: none)

**Privacy policy URL:** https://github.com/Feridoun/CallRecorder/blob/main/PRIVACY.md

**Website:** https://github.com/Feridoun/CallRecorder

**Support contact:** https://github.com/Feridoun/CallRecorder/issues

**Copyright:** © 2026 Fareedoon Ahmed

**Additional license terms:** MIT licence: https://github.com/Feridoun/CallRecorder/blob/main/LICENSE

**System requirements:** Minimum: Microphone. Windows 10 or later, x64.

## Images

- **Store logo (1:1):** `build\store\logo-1080.png`, created with
  `powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1 -StoreLogo`.
- **Screenshots (desktop, 1 to 10, PNG, at least 1366 × 768):** take them on a
  test setup, with no real recordings, tags or server addresses visible.
  Suggested shots:
  1. The tray menu open (Sensitive, Tags, Microphone, Playback)
  2. The Speakr connection dialog with an example address
  3. The "Notes ready" notification
  4. The resulting notes in Speakr, with marker links
  5. The Microphone device submenu

## Age rating (IARC questionnaire)

Category: **Utility, productivity, communication or other**. Answer **No** to
violence, fear, sexuality, gambling, language and controlled substances. Also:

- Users interact or exchange content with each other: **No** (the app doesn't
  connect users; recordings go only to the user's own server)
- Shares the user's current physical location: **No**
- Digital purchases: **No**
- Unrestricted internet access (web browser): **No**

This should come out at **3+ / Everyone**.

## Properties and declarations

- Pricing: **Free**. Markets: all.
- Tick **This product has been tested to meet accessibility guidelines** only
  after checking the connection dialog and menus with Narrator and a keyboard.
- Leave **This app collects or transmits personal information** ticked (it
  records audio and uploads it to a user-chosen server). That's why a privacy
  policy URL is needed.

## Notes for certification

Paste this into **Submission options > Notes for certification**:

> CallRecorder is a system tray app with no main window. After install, look for the grey dot in the notification area (it may be in the overflow "^" area).
>
> On first launch a "Speakr connection" dialog asks for a Speakr server address and API token. Speakr is a self-hosted transcription server (https://github.com/murtaza-nasir/speakr). You can cancel this dialog: recording works without a server.
>
> To test:
> 1. Make sure Settings > Privacy & security > Microphone > "Let desktop apps access your microphone" is on.
> 2. Press Ctrl+Alt+R (or double-click the tray icon). The icon turns red and a "Recording started" notification appears.
> 3. Speak, or play some audio, for a few seconds, then press Ctrl+Alt+R again. A "Recording saved" notification appears.
> 4. Tray menu > "Open recordings folder" shows the .opus file (plays in a browser or media player) and its .json file. Without a server, its upload_state stays "pending".
> 5. Other tray menu items: Pause (Ctrl+Alt+P), Add marker (Ctrl+Alt+K), Sensitive, Microphone and Playback device pickers, Start at login, Exit.
>
> [Optional: a test Speakr server is available at <URL> with API token <TOKEN>, valid until <DATE>.]
>
> The app contains no ads, telemetry or in-app purchases, and contacts no server except the one the user enters.
