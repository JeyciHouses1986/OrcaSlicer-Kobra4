# OrcaSlicer for the Anycubic Kobra 4 and ACE 2 Pro

> [!NOTE]
> This is an unofficial fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). It is not affiliated with, or endorsed by, OrcaSlicer or Anycubic. For OrcaSlicer itself, its documentation and how to contribute to it, see the [official repository](https://github.com/OrcaSlicer/OrcaSlicer).

This fork lets OrcaSlicer slice for the **Anycubic Kobra 4** with the **ACE 2 Pro** multi-color unit and send prints to it through the **Anycubic Cloud**, the same way Anycubic Slicer Next does, without switching the printer to LAN mode.

## What it adds

- **Kobra 4 profiles** for the 0.25, 0.4, 0.6 and 0.8 mm nozzles, with their process and filament presets.
- **The ACE header** that the Kobra 4 firmware reads from the G-code, so it knows which ACE slot feeds each filament.
- **Cloud printing**: the print dialog uploads the G-code to your Anycubic account and starts it on the printer, with the filament-to-ACE-slot mapping you choose and an optional timelapse.
- **Device page**: the printer's state, job progress, layers, remaining time and temperatures; the filament loaded in each ACE slot; and the printer's camera.
- **Printer control**: bed and nozzle temperature, fan speed, the light, axis moves and homing at any time; pause, resume and stop while a job runs.
- **ACE 2 Pro dryer**: humidity is shown for each unit and drying can be started or stopped from the device page.

## Setting it up

1. Install a build of this fork (see *Downloads* below) and add the **Anycubic Kobra 4** printer from the printer selection.
2. Open the physical printer dialog (the connection icon next to the printer), choose **Log in to Anycubic** and sign in with your Anycubic account. The sign-in token stays in OrcaSlicer's local settings on your computer.
3. In **Host**, type the printer's name as it appears in your Anycubic account. With a single printer on the account, any text works.
4. The **Device** tab then shows the printer, and **Print** sends jobs to it.

## Limitations

- Everything goes through the Anycubic Cloud, so the printer and your computer need internet access. LAN mode is not supported.
- The cloud is polled every 15 seconds, so the device page lags behind the printer by up to that long.
- Axis moves go in whole millimetres, and extruding or retracting from the device page and other manual G-code are not available through the cloud.
- Timelapses recorded by the printer cannot be downloaded from OrcaSlicer; take them from the printer's USB drive.
- The camera needs internet access to load Agora's web player, which the printer streams through.

## Downloads

There are no releases yet. Builds for Windows and Linux are produced by the [Build all](../../actions/workflows/build_all.yml) workflow and can be downloaded from the *Artifacts* of a successful run (a GitHub account is needed to download them).

## Installing on Windows

Run the installer, or unzip the portable build and start `orca-slicer.exe`. OrcaSlicer needs the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/), which recent Windows versions already include. The installer uses the same folder as the official OrcaSlicer and replaces it, and both share the same settings.

## Building from source

The build is the same as OrcaSlicer's: see [How to build](https://www.orcaslicer.com/wiki/how_to_build) on the OrcaSlicer wiki.

## License

Like OrcaSlicer, this fork is licensed under the GNU Affero General Public License, version 3: see [LICENSE.txt](LICENSE.txt). OrcaSlicer includes a pressure advance calibration pattern adapted from Andrew Ellis' generator, licensed under the GNU General Public License, version 3. The Anycubic Cloud client follows the protocol documented by the [anycubic-cloud-api](https://pypi.org/project/anycubic-cloud-api/) project (GPL-3.0-or-later).
