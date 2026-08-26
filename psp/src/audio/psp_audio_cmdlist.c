/* Interpreter for a finished N64 audio command list (Acmd), so the microcode
 * can run somewhere other than where it was built.
 *
 * WHY THIS EXISTS. The port's first working mixer redefined the a*() macros
 * in psp/include/psp_audio_mixer.h to perform each operation IMMEDIATELY, so
 * AudioSynth_Update built and executed in one pass. That made sound work, but
 * it welds the mixer to the audio thread: the expensive half cannot be moved
 * anywhere else, and it is the single largest audio cost on the main CPU.
 *
 * Splitting them back apart restores the N64 shape -- AudioSynth_Update fills
 * gAudioCtx.abiCmdBufs[] with real commands, and somebody else walks the list
 * afterwards -- which is what lets the Media Engine take over the walk (see
 * psp/src/audio/psp_audio_me.c). The reference port reference/oot-psp-z2442
 * is built the same way (OotPspMixer_ExecuteCommandList).
 *
 * Every unpack below is the exact mirror of the corresponding macro in
 * include/ultra64/abi.h. Read them as pairs: if a macro changes, the case
 * here changes with it. The a*Impl functions are unchanged -- this file only
 * decides what to call with which arguments.
 */

#include "ultra64.h"
#include "psp_audio_mixer.h"

/* Set by the ME job loop so a fault can name the command it died on. */
volatile uint32_t gPspAudioCmdIndex;
volatile uint32_t gPspAudioCmdOpcode;

void PspAudioMixer_ExecuteCommandList(const Acmd* cmdList, int32_t cmdCount) {
    int32_t i;

    if ((cmdList == NULL) || (cmdCount <= 0)) {
        return;
    }

    for (i = 0; i < cmdCount; i++) {
        uint32_t w0 = cmdList[i].words.w0;
        uint32_t w1 = cmdList[i].words.w1;
        uint32_t opcode = w0 >> 24;

        gPspAudioCmdIndex = (uint32_t)i;
        gPspAudioCmdOpcode = opcode;
        PspAudioMixer_CountCmd();

        switch (opcode) {
            case A_SPNOOP:
                break;

            /* w1 carries a full 32-bit byte count here, unlike every other
             * length in the ABI, so it still needs the clamp the immediate
             * macros applied. */
            case A_CLEARBUFF:
                aClearBufferImpl(w0 & 0xFFFF, PSP_ACMD_LEN(w1));
                break;

            case A_LOADBUFF:
                aLoadBufferImpl((const void*)(uintptr_t)w1, w0 & 0xFFFF, ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_SAVEBUFF:
                aSaveBufferImpl(w0 & 0xFFFF, (int16_t*)(uintptr_t)w1, ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_LOADADPCM:
                aLoadADPCMImpl(w0 & 0xFFFFFF, (const int16_t*)(uintptr_t)w1);
                break;

            case A_SETBUFF:
                aSetBufferImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_INTERLEAVE:
                aInterleaveImpl(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF, ((w0 >> 16) & 0xFF) << 4);
                break;

            case A_DMEMMOVE:
                aDMEMMoveImpl(w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_SETLOOP:
                aSetLoopImpl((int16_t*)(uintptr_t)w1);
                break;

            case A_ADPCM:
                aADPCMdecImpl((w0 >> 16) & 0xFF, (int16_t*)(uintptr_t)w1);
                break;

            case A_S8DEC:
                aS8DecImpl((w0 >> 16) & 0xFF, (int16_t*)(uintptr_t)w1);
                break;

            case A_RESAMPLE:
                aResampleImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (int16_t*)(uintptr_t)w1);
                break;

            case A_RESAMPLE_ZOH:
                aResampleZohImpl(w0 & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_ENVSETUP1:
                aEnvSetup1Impl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_ENVSETUP2:
                aEnvSetup2Impl((w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            /* A_ENVMIXER is the one opcode whose macro ORs a prebuilt word
             * (sEnvMixerOp) into w0 instead of a plain _SHIFTL of the opcode,
             * which is why the opcode bits and the flag bits share w0 here. */
            case A_ENVMIXER:
                aEnvMixerImpl(((w0 >> 16) & 0xFF) << 4, (w0 >> 8) & 0xFF, (w0 >> 4) & 1, (w0 >> 3) & 1,
                              (w0 >> 2) & 1, (w0 >> 1) & 1, w0 & 1, w1);
                break;

            case A_MIXER:
                aMixImpl((w0 >> 16) & 0xFF, (int16_t)(w0 & 0xFFFF), (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_ADDMIXER:
                aAddMixerImpl(((w0 >> 16) & 0xFF) << 4, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            case A_DUPLICATE:
                aDuplicateImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF);
                break;

            case A_INTERL:
                aInterlImpl((w1 >> 16) & 0xFFFF, w1 & 0xFFFF, PSP_ACMD_LEN(w0 & 0xFFFF));
                break;

            case A_FILTER:
                aFilterImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (int16_t*)(uintptr_t)w1);
                break;

            case A_HILOGAIN:
                aHiLoGainImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF);
                break;

            case A_UNK19:
                aUnkCmd19Impl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (w1 >> 16) & 0xFFFF, w1 & 0xFFFF);
                break;

            /* A_UNK3 was a no-op in the immediate macros too -- OoT never
             * emits it. Opcodes this abi.h does not even name (A_SEGMENT,
             * A_POLEF, A_PAN, A_SETVOL) land in the same place. */
            case A_UNK3:
            default:
                break;
        }
    }

    gPspAudioCmdOpcode = A_SPNOOP;
}
