#pragma once
#include "lvgl.h"

// The contract between the screen manager (Ui.cpp) and the pages. Each page builds its
// widgets once into the container it is given, at boot, and updates them in place from
// refresh(). refresh() is called a few times a second while the page is visible and once
// when it is shown; it should return quickly when nothing changed (compare a revision).

lv_obj_t *PageHome_create(lv_obj_t *parent);
void PageHome_refresh(bool force);

lv_obj_t *PageCalendar_create(lv_obj_t *parent);
void PageCalendar_refresh(bool force);

lv_obj_t *PageNetwork_create(lv_obj_t *parent);
void PageNetwork_refresh(bool force);

lv_obj_t *PageTasks_create(lv_obj_t *parent);
void PageTasks_refresh(bool force);



lv_obj_t *PageSettings_create(lv_obj_t *parent);
void PageSettings_refresh(bool force);
