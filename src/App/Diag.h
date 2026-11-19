#pragma once

// Display-glitch diagnostics: a one-line health report every 5 s on the console ("diag") and timestamped markers for the
// things that could disturb the panel's frame buffer (flash writes, Wi-Fi/ESP-NOW switches, PC link changes, calls). Look
// for what happened in the seconds before a cyan flash:
//   diag  t=...s  int free/min/largest ...  psram free ...  copies +N timeouts N slow N max Nms
//   mark  t=...s  <what>
//
// Diag_mark() is safe from any task.

/// Start the 5 s report. LVGL task, once, after Ui_begin().
void Diag_begin();

/// Log "mark t=<uptime>s <what>".
void Diag_mark(const char *what);
