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

/* Arm the automatic trigger: grab the first frames after the fixed-camera
 * background image changes. The reported corruption shows on the FIRST frame
 * of the side-view background and is gone by the second, which no hotkey can
 * catch by hand. */
extern int gPspShotOnBgChange;
void PspScreenshot_NoteBgImage(const void *img);

/* Same trigger, keyed on the active camera SETTING instead. Switching between
 * a prerendered room's two views changes the setting (CAM_SET_PREREND_FIXED ->
 * CAM_SET_PREREND_PIVOT), and the frame that goes wrong is the first one after
 * that. NoteBgImage cannot see it: it only runs while a background is being
 * blitted, and the whole point of the pivot view is that it is not. */
void PspScreenshot_NoteCamSetting(unsigned int setting);

#endif
