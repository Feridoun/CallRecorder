# Microsoft Store submission

Everything Partner Center asks for, ready to paste. MeetingRecorder is submitted
as an **EXE app**: the Store downloads the signed Inno Setup installer from
GitHub Releases and runs it silently, so no MSIX packaging is needed.

## Before each submission

1. Bump the version in `CMakeLists.txt` and `res/MeetingRecorder.manifest`, then
   build and sign with `.\tools\release.ps1 -CertThumbprint <sha1>`.
2. Publish a GitHub release tagged `v<version>` with
   `MeetingRecorder-Setup-<version>.exe` attached. The Store needs a URL that
   always serves the same file, so never replace an asset in an existing
   release; publish a new version instead.
3. In Partner Center, open the app > **Packages**, and set the package URL to
   `https://github.com/Feridoun/MeetingRecorder/releases/download/v<version>/MeetingRecorder-Setup-<version>.exe`
4. Update **What's new** in the listing, then submit.

## Packages

| Field | Value |
|---|---|
| Package URL | `https://github.com/Feridoun/MeetingRecorder/releases/download/v1.3.0/MeetingRecorder-Setup-1.3.0.exe` |
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
| 7 | MeetingRecorder is running and didn't exit when asked. Exit it from the tray and try again (custom) |
| 8 | Reboot required |
| 1, 3, 4 | Setup failed (miscellaneous) |

## Store listing (English)

**Product name:** MeetingRecorder
If the name is taken, try `MeetingRecorder for Speakr` or `Speakr Meeting Recorder`.

**Short description** (1,000 characters max):

> A companion app for people who run, or have access to, a Speakr server (self-hosted transcription). Record both sides of any call or online meeting (Teams, Zoom, Google Meet, WhatsApp, a softphone) with one hotkey, and MeetingRecorder uploads it to your Speakr server for a transcript and summary. It also keeps recordings on your PC, so you can play them or keep them local-only.

**Description** (10,000 characters max):

> MeetingRecorder is for people who run, or have access to, a Speakr server. Speakr (https://github.com/murtaza-nasir/speakr) is free, self-hosted transcription software. MeetingRecorder is a small tray app that records calls and online meetings and sends them to that server for transcripts and summaries. If you don't use Speakr, you can still use MeetingRecorder to record calls to your PC and play them back, but the transcripts and summaries come from Speakr, and MeetingRecorder doesn't make them itself.
>
> It records both sides of a call without a virtual audio cable: your microphone, mixed with whatever your speakers or headset are playing. That works with Teams, Zoom, Google Meet, WhatsApp, softphones and any other calling app. Press Ctrl+Alt+R to start and again to stop. A notification tells you when Speakr has the notes ready.
>
> TINY AND NATIVE
> A single signed app of about 1 MB, written in C++. No runtime, no background services, no window to manage.
>
> CRASH-PROOF RECORDINGS
> Audio is written to disk as it's recorded, in Opus format (about 11 MB per hour). A crash or power cut loses under half a second, and the interrupted recording is still uploaded.
>
> SENSITIVE MODE AND LOCAL RECORDINGS
> One click keeps a recording on this PC only. It's never uploaded. You can also start in local-only mode, or choose "Keep last recording on this PC" during the upload delay. The Recordings window lists everything on your PC, with Play, Open in Speakr, Upload, Keep on this PC and Delete.
>
> A MOMENT TO CHANGE YOUR MIND
> Recordings wait 60 seconds (you can change this) before they upload. Nothing recorded before you set up a server, or for a different server, is sent without asking you.
>
> RECORD JUST WHAT YOU WANT
> Choose "Only record audio from these apps" (for example Teams.exe) so other sounds on your PC stay out of the recording. Needs a recent Windows 10 or 11; if it can't start, MeetingRecorder warns you and records all playback audio. Or record you and the others on separate stereo channels for better speaker labels.
>
> MARKERS AND TAGS
> Press Ctrl+Alt+K at a key moment. In Speakr's notes, each marker links to that point in the audio. Pick Speakr tags from the tray menu so Speakr applies the right summary prompt.
>
> RELIABLE UPLOADS
> Uploads are queued and retried while your server is unreachable (for example when you're offline or the VPN is down).
>
> PRIVATE BY DESIGN
> No accounts, analytics or telemetry. MeetingRecorder only talks to the Speakr server you choose, and keeps your API token in Windows Credential Manager.
>
> REQUIREMENTS
> A Speakr server you can reach (https://github.com/murtaza-nasir/speakr) to get transcripts and summaries. It is not included and isn't part of this app. Without one, MeetingRecorder only records to your PC.
>
> GOOD TO KNOW
> MeetingRecorder records your microphone even if you're muted in your call app. Use Pause or Sensitive mode for private moments. Plain http:// addresses send recordings and your API token unencrypted, and Settings warns you about them.
>
> PLEASE RECORD LAWFULLY
> The rules on recording calls vary by country and state, and many places require everyone on the call to consent. Tell people you're recording.
>
> MeetingRecorder is free and open source (MIT licence) and isn't affiliated with Speakr.

**What's new in this version:**

> New in 1.2:
> - Recordings window: see every recording, play it, open it in Speakr, upload it, keep it on this PC or delete it.
> - Upload delay: recordings wait 60 seconds (adjustable) before uploading, and "Keep last recording on this PC" stops the upload.
> - Held recordings: nothing recorded before you set up Speakr, or for another server, is sent without asking.
> - Only record audio from chosen apps (for example Teams.exe), so other sounds stay out.
> - Optional separate channels: you on the left, everyone else on the right.
> - Warnings for blocked microphone access and a silent microphone.
> - Pause cuts exactly when you press it, and loud audio is limited softly instead of clipping.
> - Settings no longer freezes while testing a slow connection.
> - Many fixes: no crash on a damaged settings file, local audio is no longer deleted if Speakr loses a recording, no duplicate uploads after an interruption, and recordings can't overwrite each other after a clock change.
> - Warning: MeetingRecorder records your microphone even when you're muted in Teams or Zoom. Use Pause or Sensitive mode.

**Product features** (up to 20, 200 characters each):

1. For people with a Speakr server: uploads recordings for transcripts and summaries
2. Records both sides of any call: your microphone plus your PC's playback audio
3. One hotkey to start and stop (Ctrl+Alt+R), plus pause and markers
4. Sensitive mode keeps a recording on this PC only
5. Recordings window: play, upload, keep on this PC or delete any recording
6. Upload delay with "Keep last recording on this PC" to change your mind
7. Only record audio from the apps you choose, or use separate stereo channels
8. Crash-proof: audio is saved as it's recorded
9. Choose the microphone and playback device, or follow the Windows defaults
10. Uploads queued and retried while offline
11. No accounts, analytics or telemetry

**Search terms** (7 max):
`call recorder`, `meeting recorder`, `teams recorder`, `zoom recorder`, `speakr`, `transcription`, `voice recorder`

**Category:** Productivity (subcategory: none)

**Privacy policy URL:** https://github.com/Feridoun/MeetingRecorder/blob/main/PRIVACY.md

**Website:** https://github.com/Feridoun/MeetingRecorder

**Support contact:** https://github.com/Feridoun/MeetingRecorder/issues

**Copyright:** © 2026 Fareedoon Ahmed

**Additional license terms:** MIT licence: https://github.com/Feridoun/MeetingRecorder/blob/main/LICENSE

**System requirements:** Minimum: Microphone. Windows 10 or later, x64.

## Images

- **Store logo (1:1):** `build\store\logo-1080.png`, created with
  `powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1 -StoreLogo`.
- **Screenshots (desktop, 1 to 10, PNG, at least 1366 × 768):** take them on a
  test setup, with no real recordings, tags or server addresses visible.
  Suggested shots:
  1. The tray menu open (Sensitive, Tags, Recordings, Settings)
  2. The Recordings window, with only test recordings
  3. The Settings window with an example address
  4. The "Notes ready" notification
  5. The resulting notes in Speakr, with marker links

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

> MeetingRecorder is a system tray app with no main window. After install, look for the grey dot in the notification area (it may be in the overflow "^" area).
>
> On first launch the Settings window opens and asks for a Speakr server address and API token. Speakr is a self-hosted transcription server (https://github.com/murtaza-nasir/speakr). You can cancel this dialog: recording works without a server.
>
> To test:
> 1. Make sure Windows Settings > Privacy & security > Microphone > "Let desktop apps access your microphone" is on.
> 2. Press Ctrl+Alt+R (or double-click the tray icon). The icon turns red and a "Recording started" notification appears.
> 3. Speak, or play some audio, for a few seconds, then press Ctrl+Alt+R again. A "Recording saved" notification appears.
> 4. Tray menu > "Recordings..." lists the recording, and Play opens it in the default player. "Open recordings folder" shows the .opus file and its .json file. Without a server, its upload_state stays "pending" and nothing is uploaded.
> 5. Other tray menu items: Pause (Ctrl+Alt+P), Add marker (Ctrl+Alt+K), Sensitive, Keep last recording on this PC, Settings (microphone and playback device pickers, Start at login), Exit.
>
> [Optional: a test Speakr server is available at <URL> with API token <TOKEN>, valid until <DATE>.]
>
> The app contains no ads, telemetry or in-app purchases, and contacts no server except the one the user enters.
