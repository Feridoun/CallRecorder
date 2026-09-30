# CallRecorder privacy policy

Last updated: 30 September 2026

CallRecorder is a Windows app that records calls and meetings on your PC and,
if you choose, sends them to a [Speakr](https://github.com/murtaza-nasir/speakr)
server that **you** run or have access to. This policy explains what the app
does with your data.

## The short version

- CallRecorder has no accounts, analytics, telemetry, advertising or crash
  reporting. The developer receives nothing from the app.
- It only connects to the Speakr server address you enter. It contacts no
  other server.
- Recordings stay on your PC until they're uploaded to that server. Recordings
  marked **Sensitive** are never uploaded.
- Nothing recorded before you set up a server, or for a different server, is
  sent without asking you first.

## What the app accesses

**Microphone and playback audio.** CallRecorder records your microphone and the
audio your PC is playing (the other side of a call), but only between when you
start a recording and when you stop it. The tray icon turns red (or purple in
Sensitive mode) while it's recording. CallRecorder records your microphone even
if you've muted yourself in a call app, because their mute doesn't reach it.
Use Pause or Sensitive mode for anything private.

If you list apps under **Only these apps** in Settings, CallRecorder records
only the audio those apps play, which keeps other sounds on your PC (music,
other calls, notifications) out of the recording. Your microphone is still
recorded.

**Which apps are in a call.** Unless you turn **Detect calls** off in
Settings, CallRecorder checks every 2 seconds which apps are using a
microphone (the same list the Windows Volume Mixer shows). While a call app or
browser is using one, it also reads the titles of that app's windows to find
the meeting's name. It doesn't read the audio of other apps and doesn't record
until you or your settings say so. None of this is stored, except the meeting's
name in the title of a recording you make, and none of it is sent anywhere
apart from that title going to Speakr with the recording.

**Your Speakr address and API token.** The address is saved in
`%APPDATA%\CallRecorder\config.json`. The token is saved in Windows Credential
Manager, never in a file.

## What is stored on your PC

Each recording is saved in `%LOCALAPPDATA%\CallRecorder\sessions\` as an audio
file (`.opus`) and a small `.json` file. The `.json` file holds the start time,
title, tags, markers and upload status. After Speakr has finished processing a
recording, its local audio is deleted once it's older than the number of days
set in `keep_audio_days` (14 by default; 0 keeps it forever). Sensitive, held,
failed, stuck and missing recordings are never deleted automatically. You can
delete any recording yourself from the Recordings window. Uninstalling the app
leaves these files in place, so you can delete them yourself.

If a crash leaves a recording without readable details, CallRecorder recovers
it on the next start and keeps it on your PC only. It is uploaded only if you
choose to upload it.

## What is sent, and where

When a recording that isn't marked Sensitive ends, CallRecorder waits for a
grace period (60 seconds by default, set by `upload_delay_seconds`) and then
uploads it to the Speakr server you set up. During the grace period you can
choose **Keep last recording on this PC** in the tray menu, and it is never
uploaded.

A recording is only sent to the server it was made for. If you made it before
you set up a server, or for a different server address, CallRecorder holds it
on your PC and asks before uploading it anywhere.

The upload goes over the connection you set up. Use `https://` for encryption
in transit: with plain `http://`, your recordings and API token cross the
network unencrypted. Settings warns you about `http://` addresses that aren't
on a private or local network. The upload includes:

- the audio file
- the recording's title and start time
- markers, tags and any hotwords you set in `config.json`

The app also asks that server for your list of tags and for the processing
status of each uploaded recording.

After that, the recording is handled by your Speakr server under that server's
settings. The server might transcribe it on the same machine, or send it to a
third-party speech-to-text or AI service. Whoever runs the server is
responsible for that processing. If the server is run by your employer or
someone else, their privacy policy applies to your recordings once they're
uploaded.

## Sharing

The developer receives no data from CallRecorder and so shares none. The app
never sends data anywhere except the server you configure.

## Recording other people

Laws on recording calls differ by country and state, and many places require
everyone on the call to agree. You're responsible for telling people you're
recording them and getting any consent the law requires. CallRecorder doesn't
announce itself to other people on the call.

## Children

CallRecorder isn't directed at children and doesn't knowingly collect data
from anyone.

## Changes

Changes to this policy are published in this file, and its history is
available on GitHub.

## Contact

Questions or concerns: open an issue at
<https://github.com/Feridoun/CallRecorder/issues>.
