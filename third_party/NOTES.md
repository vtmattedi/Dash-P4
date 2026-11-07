# third_party

Code taken from elsewhere and kept as it came, so the build does not depend on a fork we cannot fetch.

## esp_cam_sensor

Espressif's camera sensor drivers (Apache-2.0), **the copy from the Guition vendor package**
(`JC1060P470C_I_W_Y/1-Demo/Demo_IDF/ESP-IDF_5.5.4/JC1060P470C_I_W_Y_New_Panel/video_lcd_display/components/esp_cam_sensor`,
version 2.1.0, `repository_info.commit_sha` 70d2ce41...). Only `test_apps/` and `CHECKSUMS.json` were removed.

**Why not the registry's `espressif/esp_cam_sensor`:** none of its releases (2.0.1 to 2.6.0, checked 2026-11-06) contains an
OV02C10 driver, and OV02C10 is the sensor on this board's camera module. This copy has it (`sensors/ov02c10/`, 1288x728 and
1920x1080 modes, 1 or 2 lanes, tuning files for P4 chip revisions eco4 and eco5).

**How it takes the registry's place:** a local component named `esp_cam_sensor` (EXTRA_COMPONENT_DIRS in the root
CMakeLists.txt) overrides the managed dependency `espressif/esp_cam_sensor` that `esp_video` asks for, which is what the
vendor's own demo does. `esp_video` is the 2.1 generation, the one this copy belongs to (see the patched copy below).

**To drop it:** when a registry release gains an OV02C10 driver, remove this folder and its line in the root CMakeLists.txt,
and move `esp_video` to the matching release.

## esp_video (patched)

Espressif's V4L2-style camera framework, version 2.1.0 (commit 70d2ce41..., Apache-2.0), with `examples/`, `test_apps/` and
`CHECKSUMS.json` removed and **one addition**: a user-adjustment layer in `src/esp_video_isp_pipeline.c`, declared in
`include/esp_video_isp_user.h` (every changed spot is marked `Dash-P4`).

**Why it is patched:** the ISP pipeline controller runs the tuning loop (esp_ipa) every frame, and the loop rewrites
saturation, contrast and the white-balance gains each cycle (and exposure/gain), so values written straight to the ISP are
gone a frame later. The addition applies the user's say on top of what the loop computed, in the same cycle:
- saturation and contrast: scale the tuned values; brightness: an offset (the ISP applies it after the exposure statistics);
- white balance: automatic, or hold fixed gains (the loop's gains in effect at that moment, optionally shifted warmer/cooler);
- exposure compensation: the brightness measurements the auto-exposure loop sees are scaled by 2^-EV, so it settles at a
  brighter or darker real exposure.
With the default user settings the code writes exactly what the unpatched one would.

**How it takes the registry's place:** a local component named `esp_video` (EXTRA_COMPONENT_DIRS in the root CMakeLists.txt, camera
envs only). Its manifest still asks the component manager for esp_ipa (and cmake_utilities); `esp_cam_sensor` is the local copy above.

**To drop it:** if a registry release of esp_video offers a runtime hook for the tuning loop's outputs, remove this folder, its line in
the root CMakeLists.txt, and re-add `espressif/esp_video` to camera/idf_component.yml; camera.c is the only caller.

**Manifest change:** `esp_h264` and `usb_host_uvc` are removed from `third_party/esp_video/idf_component.yml`. Their code is only compiled when
the H.264 or USB-UVC video device is enabled in Kconfig (neither is). Reason: PlatformIO passes every linker-fragment path on the command line
of `ldgen`, the dashboard-with-camera env came to 8,164 characters against cmd.exe's 8,191 ("The command line is too long" at "Generating project
linker script"), and every component with a `linker.lf` adds ~130 characters. If a future change adds components to that env and the error
returns: drop optional components first, or shorten the PlatformIO package path.
