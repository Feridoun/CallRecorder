# WinGet manifests

The manifests under `manifests/` are submitted to
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) so people can
`winget install Feridoun.CallRecorder`. They point at the signed installer on
GitHub Releases, so that release must be published first.

## First submission

1. Publish the GitHub release `v<version>` with `CallRecorder-Setup-<version>.exe`.
2. Check the manifest and the installer:

   ```powershell
   winget validate --manifest winget\manifests\f\Feridoun\CallRecorder\<version>
   # Optional, on a test machine (it closes a running CallRecorder). Needs
   # "winget settings --enable LocalManifestFiles" from an admin prompt once.
   winget install --manifest winget\manifests\f\Feridoun\CallRecorder\<version>
   ```

3. Fork microsoft/winget-pkgs, copy the version folder to the same path
   (`manifests/f/Feridoun/CallRecorder/<version>/`), and open a pull request.
   A bot runs validation and a Defender scan, then a moderator merges it,
   usually within a few days.

## New versions

`wingetcreate` builds the new manifests from the previous ones, fills in the
hash and opens the pull request:

```powershell
winget install Microsoft.WingetCreate
wingetcreate update Feridoun.CallRecorder --version <version> `
    --urls https://github.com/Feridoun/CallRecorder/releases/download/v<version>/CallRecorder-Setup-<version>.exe `
    --submit
```

Then update `ReleaseNotes` in winget-pkgs, and copy the new folder here to keep
a record. The installer hash must match the published file, so don't rebuild
or re-sign a release after publishing it.
