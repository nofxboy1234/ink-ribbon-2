#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>

typedef struct Editor Editor;

/* Loads startup_path (or the demo level) and fits the camera on the first
   frame. Returns NULL only when allocation fails. */
Editor* editor_create(const char* startup_path);
void editor_destroy(Editor* editor);

/* Loads path as the external image onion skin, replacing any previous image.
   Returns false and reports the failure in the status line. */
bool editor_load_onion_image(Editor* editor, const char* path);

/* Builds and handles the complete editor UI for one ImGui frame. Call after
   simgui_new_frame and before simgui_flush. */
void editor_frame(Editor* editor);

#endif
