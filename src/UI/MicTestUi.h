#pragma once

/// Temporary bring-up screen for the board microphone: a "recording" dialog for 5 seconds, then the levels per half second as
/// bars, and a plain verdict (heard sound or not). Settings > Diagnostics > Microphone test. LVGL task only.
void MicTestUi_open();
