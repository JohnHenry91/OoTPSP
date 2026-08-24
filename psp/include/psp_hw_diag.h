#ifndef PSP_HW_DIAG_H
#define PSP_HW_DIAG_H

/* See psp/src/psp_hw_diag.c. Writes a durable boot trace and a crash report
 * next to EBOOT.PBP, because on real hardware there is no debugger to attach
 * and a console that switches itself off leaves nothing else behind. */
void PspDiag_Init(const char* baseDir);
void PspDiag_Step(const char* step);
void PspDiag_Frame(unsigned int n);
void PspDiag_Hex(const char* label, const void* addr, int words);
void PspDiag_Note(const char* fmt, unsigned int a, unsigned int b);

#endif
