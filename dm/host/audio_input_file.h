// A recording as the host's audio input, for tests (Mac): DM_MIC_FILE=<file> plays it to a game's input streams in place of a microphone,
// in real time from its start each time the capture starts, then silence. No device is opened and no microphone permission is involved.
// The file is anything AudioToolbox reads (WAV, M4A, MP3, …; its channels averaged, resampled to 48 kHz), or a .pcm / .raw file of 16-bit
// mono samples at 48 kHz, which is what DM_MIC_DUMP writes. audio_input_mac.m routes its platform hooks here while DM_MIC_FILE is set.
#ifndef DM_AUDIO_INPUT_FILE_H
#define DM_AUDIO_INPUT_FILE_H

int dm_mic_file_selected(void);   // DM_MIC_FILE is set
int dm_mic_file_prepare(void);    // reads the whole file into memory, again only when DM_MIC_FILE names another file; 0 on success
int dm_mic_file_start(void);      // a thread hands it to dm_audio_input_process, 512 frames at a time at the pace of a 48 kHz device
void dm_mic_file_stop(void);
void dm_mic_file_close(void);
void dm_mic_file_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels);

#endif
