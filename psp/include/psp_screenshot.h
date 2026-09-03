#ifndef PSP_SCREENSHOT_H
#define PSP_SCREENSHOT_H

/* Frame grabber, for reporting rendering bugs with a picture instead of a
 * description.
 *
 * This exists because the corruption classes this port keeps producing are
 * told apart by what they LOOK like, not by any counter: byte-order errors
 * come out as per-texel confetti, a missed cache writeback as horizontal
 * bands of old and new, a wrong row stride as a diagonal shear, and a stray
 * pointer as structured noise. Four different bugs, one word ("kaputt") to
 * describe them all -- and the port's history is full of days lost to
 * chasing the wrong one of the four.
 *
 * Writes 24-bit BMP next to EBOOT.PBP as shotNNN.bmp. BMP rather than PNG
 * because it needs no compressor, and 24-bit rather than the framebuffer's
 * own RGB565 because the file should be openable anywhere without anyone
 * having to know what the PSP's pixel format is.
 */

/* Grab the next `frames` finished frames. Queued rather than immediate: the
 * interesting frame is usually the first one after something changed, and a
 * button press cannot be timed to a single frame. */
void PspScreenshot_Request(int frames);

/* Called once per frame from the renderer, after the GE has finished and
 * before the buffer is swapped away. Does nothing unless frames are queued. */
void PspScreenshot_Tick(const void *fb565, int width, int height, int stride);

/* Shots written so far, for the HUD -- a capture that silently fails to open
 * its file looks exactly like a hotkey that never fired. */
unsigned int PspScreenshot_StatCount(void);
unsigned int PspScreenshot_StatFails(void);
/* sceIoOpen-Rueckgabewert des letzten fehlgeschlagenen Versuchs, und ob der
 * Ausweichpfad ms0:/ benutzt wurde. Damit "es passiert nichts" diagnostizierbar
 * wird statt ratbar. */
int PspScreenshot_StatLastErr(void);
unsigned int PspScreenshot_StatFallback(void);
/* Automatic grabs still allowed this session; 0 means the budget is spent and
 * only the manual hotkey will produce anything. */
int PspScreenshot_StatAutoBudget(void);
/* Automatic triggers that fired, whether or not a file resulted. Separates
 * "the trigger never ran" from "it ran and nothing was written". */
unsigned int PspScreenshot_StatAutoFired(void);

/* Grab the first frames of a newly loaded scene. */
void PspScreenshot_NoteSceneLoad(void);

/* Grab the first frames after a ROOM change within the same scene --
 * Room_ProcessRoomRequest applying a newly DMA'd room, e.g. walking through a
 * door between two rooms of one dungeon/house. NoteSceneLoad does not cover
 * this: Play_Init only runs once, when the SCENE changes, and most rooms are
 * never seen by it at all. This is the trigger that also fires for the first
 * room of a new scene (Room_ProcessRoomRequest runs there too), so it
 * overlaps NoteSceneLoad rather than replacing it -- redundant on that one
 * event, but the only coverage for every room boundary after it. */
void PspScreenshot_NoteRoomChange(void);

/* The automatic trigger, OFF by default and limited to a handful of grabs
 * per session even once armed (PspScreenshot_StatAutoBudget reports what is
 * left). Toggled from the hack menu (SELECT -> HACKS -> "Auto screenshot on
 * scene/room/camera change"), not a hotkey -- see the definition in
 * psp_screenshot.c for why a hotkey stopped being the right home for this.
 * Grabs the first frames after a fixed-camera background image changes: the
 * reported corruption shows on the FIRST frame of the side-view background
 * and is gone by the second, which no hand-timed hotkey could catch anyway. */
extern int gPspShotOnBgChange;
void PspScreenshot_NoteBgImage(const void *img);

/* Same trigger, keyed on the active camera SETTING instead. Switching between
 * a prerendered room's two views changes the setting (CAM_SET_PREREND_FIXED ->
 * CAM_SET_PREREND_PIVOT), and the frame that goes wrong is the first one after
 * that. NoteBgImage cannot see it: it only runs while a background is being
 * blitted, and the whole point of the pivot view is that it is not. */
void PspScreenshot_NoteCamSetting(unsigned int setting);

#endif
