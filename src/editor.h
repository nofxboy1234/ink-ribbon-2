#ifndef EDITOR_H
#define EDITOR_H

typedef struct Editor Editor;

/* Loads startup_path (or the demo level) and fits the camera on the first
   frame. Returns NULL only when allocation fails. */
Editor* editor_create(const char* startup_path);
/* 0 is the shipped editor, 1..3 are prototype variants a, b, c. */
void editor_destroy(Editor* editor);

/* Builds and handles the complete editor UI for one ImGui frame. Call after
   simgui_new_frame and before simgui_flush. */
void editor_frame(Editor* editor);

#endif
