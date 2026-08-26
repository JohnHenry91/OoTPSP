#include "psp_audio_stage.h"

#include <pspiofilemgr.h>
#include <string.h>

#include "psp_blob_assets.h"

int gPspAudioStage = PSP_AUDIO_STAGE_FULL;

void PspAudioStage_Init(void) {
    char path[256];
    char buf[16];
    const char* base = PspBlob_GetBaseDir();
    SceUID fd;
    int n;
    int value = 0;
    int digits = 0;
    int i;

    path[0] = '\0';
    if ((base != NULL) && (base[0] != '\0') && (strlen(base) + sizeof("audiostage.txt") <= sizeof(path))) {
        strcpy(path, base);
        strcat(path, "audiostage.txt");
        fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    } else {
        fd = sceIoOpen("audiostage.txt", PSP_O_RDONLY, 0777);
    }

    if (fd < 0) {
        return; /* normal case: no file, full game */
    }

    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    for (i = 0; i < n; i++) {
        if ((buf[i] >= '0') && (buf[i] <= '9')) {
            value = (value * 10) + (buf[i] - '0');
            digits++;
        } else if (digits > 0) {
            break;
        }
    }

    /* A file that exists but holds no digit is a typo, not a request to run a
     * crippled game -- leave the full stage rather than silently muting. */
    if (digits == 0) {
        return;
    }
    if (value > PSP_AUDIO_STAGE_MAX) {
        value = PSP_AUDIO_STAGE_MAX;
    }
    gPspAudioStage = value;
}
