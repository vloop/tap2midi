// Tap 2 MIDI
// Marc Perilleux 2019
// Detect taps on microphone or other sound input and triggers MIDI notes (velocity-sensitive)

/*
 *      This program is free software; you can redistribute it and/or modify it
 *      under the terms of the GNU General Public License as published by the
 *      Free Software Foundation; either version 2 of the License,
 *      or (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 *
*/

// Includes code from http://equalarea.com/paul/alsa-audio.html Minimal Capture Program

// Compile with:
// gcc tap2midi.c -lasound -lm -o tap2midi

// Use example (maybe a bit conservative):
// wait time 8ms, trigger level -24 db
// ./tap2midi -D hw:3,0 -d 0.98 -t 8 -l -24
// A little more responsive:
// -d 0.98 -t 0 -l -30
// Method 2:
// ./tap2midi -D hw:2,0 -t 2 -w 25 -l -12
// NB - to identify your soundcard (hw:3,0 above), use
// arecord -l

// Currently hard-coded to S24_3LE sample format
// int must be at least 32 bits

// Method 1:
// Debouncing uses 2 different mechanisms, which can be combined:
// - set a certain delay time before retriggering is allowed
//   use parameter -t followed by milliseconds
// - trigger only if level exceeds a decreasing envelope
//   use parameter -d followed by decay rate per buffer FIXME
//   and parameter -g guard factor (envelope overshoot) FIXME

// Method 2:
// When trigger level is reached, detect peak within t ms
// After peak detection, wait for w ms before re-triggering is allowed

// TODO list
// flush stdout at every printf
// OSC for individual audio channel parameters, including midi settings
// Gui to send OSC messages, save and read file

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <math.h>



#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
       _a > _b ? _a : _b; })
#define min(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
       _a > _b ? _b : _a; })

// #define debug

#define buf_frames (128)
// #define channels (2)
// #define channel_bytes (3)
// #define frame_bytes (channels*channel_bytes)
// #define buf_bytes (buf_frames*frame_bytes)

static volatile int keepRunning = 1;

int verbose = 0;

// See https://www.alsa-project.org/alsa-doc/alsa-lib/_2test_2rawmidi_8c-example.html
snd_rawmidi_t *handle_out = 0;

void intHandler(int dummy) {
    keepRunning = 0;
    fprintf (stderr, "Interrupted!\n");
}

void send_note_on(int channel, int note, int velocity){
    // send midi note on or off message
    // velocity = 0 means note off
    unsigned char ch[3];
    ch[0] = 0x90 + (channel & 0x0F);
    ch[1] = note & 0x7F;
    ch[2] = velocity & 0x7F;
    if (verbose){
        if (ch[2]){
            fprintf(stderr, "\nMIDI note on %x %x %x ", (unsigned int)ch[0], (unsigned int)ch[1], (unsigned int)ch[2]);
        }else{
            fprintf(stderr, "\nMIDI note off %x %x ", (unsigned int)ch[0], (unsigned int)ch[1]);
        }
    }
    snd_rawmidi_write(handle_out, ch, 3);
    //~ snd_rawmidi_write(handle_out, &ch[0], 1);
    //~ snd_rawmidi_write(handle_out, &ch[1], 1);
    //~ snd_rawmidi_write(handle_out, &ch[2], 1);
    snd_rawmidi_drain(handle_out); // Not always effective??
}

void send_note_off(int channel, int note){
    send_note_on(channel, note, 0);
}

void (*f)(int channel_count, char *buf2, int *max_l, int *previous_max_l, int *previous_max_v);

void find_peak_S16_LE(int channel_count, char *buf, int *max_l, int *previous_max_l, int *previous_max_v){
    // The following section is hard-coded to S16_LE
    // Look for peak
    int c;
    for(c = 0; c < channel_count; c++){
        previous_max_v[c] = (previous_max_l[c]-1) >> 8; // 16-bit specific
        // 7 MSB; -1 in case previous_max_l is 0x8000000 (abs(-0x8000000)));
    }
    int frame;
    for(frame = 0; frame < buf_frames; frame++){
        for(c = 0; c < channel_count; c++){
            int l, a, d;
            //~ a = buf[byte_offset] | buf[byte_offset+1]<<8 | buf[byte_offset+2]<<16; // Unsigned
            a = ((short int*)buf)[0]; // buf[0] | buf[1]<<8;
            l = abs(a);
            //~ d = abs(a - previous[c]);
            //~ previous[c] = a;
            if (l > max_l[c]){
                max_l[c] = l;
            }
            //~ if (d > max_d[c]){ // Max diff ~ slope - should help predicting hit strength ?
                //~ max_d[c] = d;
            //~ }
            buf += 2; //channel_bytes; // byte_offset = frame_bytes * frame + channel_bytes * c;
        }
    }
}

void find_peak_S24_3LE(int channel_count, char *buf, int *max_l, int *previous_max_l, int *previous_max_v){
    // The following section is hard-coded to S24_3LE
    // Look for peak
    int c;
    for(c = 0; c < channel_count; c++){
        previous_max_v[c] = (previous_max_l[c]-1) >> 16; // 24-bit specific
        // 7 MSB; -1 in case previous_max_l is 0x8000000 (abs(-0x8000000)));
    }
    int frame;
    for(frame = 0; frame < buf_frames; frame++){
        for(c = 0; c < channel_count; c++){
            int l, a, d;
            //~ a = buf[byte_offset] | buf[byte_offset+1]<<8 | buf[byte_offset+2]<<16; // Unsigned
            a = buf[0] | buf[1]<<8 | buf[2]<<16; // Unsigned
            l = a;
            if (a > 0x7FFFFF){ // Sample is negative
                a = a-0x1000000; // Get signed amplitude, -800000..-1
                l = -a; // abs, 1..800000
            }
            //~ d = abs(a - previous[c]);
            //~ previous[c] = a;
            if (l > max_l[c]){
                max_l[c] = l;
            }
            //~ if (d > max_d[c]){ // Max diff ~ slope - should help predicting hit strength ?
                //~ max_d[c] = d;
            //~ }
            buf += 3; // channel_bytes; // byte_offset = frame_bytes * frame + channel_bytes * c;
        }
    }
}

// Method 2 helper functions
int (*f_peak)(int channel_count, char *buf, int frame_count, int channel, int *peak);

int find_channel_peak_S16_LE(int channel_count, char *buf, int frame_count, int channel, int *peak){
    int frame, peak_frame;
    peak_frame=-1;
    int a;
    for(frame = 0; frame < frame_count; frame++){
		a=abs(((short int*)buf)[frame * channel_count + channel]);
		if (a > *peak){
			*peak=a;
			peak_frame=frame;
		}
	}
	return(peak_frame);
}
 
int find_channel_peak_S24_3LE(int channel_count, char *buf, int frame_count, int channel, int *peak){
    int frame, frame_bytes, peak_frame;
    int a;
    frame_bytes = 3 * channel_count;
    peak_frame=-1;
    for(frame = 0; frame < frame_count; frame++){
		a = buf[channel] | buf[channel+1]<<8 | buf[channel+2]<<16; // Unsigned)
		if (a > 0x7FFFFF){ // Sample is negative
			a = 0x1000000 - a ; // abs		
		}
		if (a > *peak){
			*peak=a;
			peak_frame=frame;
		}
		buf += frame_bytes;
	}
	return(peak_frame);
}

int (*f_trig)(int channel_count, char *buf, int frame_count, int channel, int trig_lvl);

int find_channel_trig_S16_LE(int channel_count, char *buf, int frame_count, int channel, int trig_lvl){
    int frame;
    int a;
    for(frame = 0; frame < frame_count; frame++){
		a=abs(((short int*)buf)[frame * channel_count + channel]);
		if (a>trig_lvl) return frame;
	}
	return(-1);
}
 
int find_channel_trig_S24_3LE(int channel_count, char *buf, int frame_count, int channel, int trig_lvl){
    int frame, frame_bytes;
    int peak = 0;
    int a, l;
    frame_bytes = 3 * channel_count;
    for(frame = 0; frame < frame_count; frame++){
		a = buf[channel] | buf[channel+1]<<8 | buf[channel+2]<<16; // Unsigned)
		if (a > 0x7FFFFF){ // Sample is negative
			a = 0x1000000 - a ; // abs		
		}
		if (a>trig_lvl) return frame;
		buf += frame_bytes;
	}
	return(-1);
}

void usage(char *prog_name){
    printf("Usage: %s [OPTION]...\n\n", prog_name);
    printf("-c channels channel count\n");
    printf("-d rate     envelope decay rate (per buffer)\n");
    printf("            typically 0.97..0.99, higher values mean more anti-bouncing\n");
    printf("-D device   alsa sound input device\n");
    printf("-f          faster slope detection (may cause double-triggering)\n");
    printf("-g factor   initial gain of envelope (db)\n");
    printf("            typically 0, higher values mean more anti-bouncing\n");
    printf("-h          display this help message\n");
    printf("-l level    trigger level (db, must be negative)\n");
    printf("            typically -36..-24, more negative values mean more sensitivity\n");
    printf("-r rate     sample rate (Hz)\n");
    printf("-t time     trigger delay time (ms)\n");
    printf("-w time     retrigger wait delay time for anti-bouncing (ms)\n");
    printf("-v          verbose\n");
    printf("-x time     note off (extinction) delay time (ms)\n");
    printf("-X          force note off (extinction) before new note\n");
}

int main (int argc, char *argv[])
{
    int i;
    int err;
    int errcount=0;
    char *device_name = "default";
    int sample_rate = 44100; // Will be updated by ALSA
    int channels = 2, channel_bytes, frame_bytes, buf_bytes;
    int max_sample_value = 0x7FFFFF; // 24SE -> 3 bytes per sample
    unsigned char* buf; //[buf_bytes];
    char bidon;
    snd_pcm_t *capture_handle;
    snd_pcm_hw_params_t *hw_params;
    float trig_delay_ms = 0, wait_delay_ms = 0;
    int trig_delay_frames_default, trig_delay_buffers_default;
    int wait_delay_frames_default, wait_delay_buffers_default;
    float decay_rate_default = 0.98;
    float decay_factor_default;
    float decay_factor_db = 6.0;
    float trigger_level_db = -30.0; // full range 0x7FFFFF / 0x7FFF = 256 ==> -48db = -6 * ln(256)/ln(2)
    float max_note_off_delay_ms = 250.0;
    int force_note_off = 0;
    int single_buffer = 0;
    int trig_level_default;
    // ln(q)=G * ln(2)/-6 ==> q = exp(G * ln(2)/-6)

    // Handle command-line arguments
    int arg = 1;
    //~ if (argc>1){
        //~ device_name = argv[1];
        //~ arg++;
    //~ }
    while(arg<argc){
        // printf("%s\n", argv[arg]);
        if (argv[arg][0]!='-'){
            fprintf(stderr, "%s: not an option.\n", argv[arg]);
            errcount++;
        }else{ // Found dash, we know [1] is not past end of string
            if ( argv[arg][1] && (argv[arg][2] == 0)){ // Length is ok
                switch(argv[arg][1]){
                    case 'h':
                        usage(argv[0]);
                        exit(0);
                    case 'v':
                        verbose++;
                        break;
                    case 'r': // Sample rate
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%d%c", &sample_rate, &bidon) != 1) {
                                fprintf(stderr, "%s: not an integer.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'c': // channel count
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%d%c", &channels, &bidon) != 1) {
                                fprintf(stderr, "%s: not an integer.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'd': // decay value
                        // see https://tomroelandts.com/articles/low-pass-single-pole-iir-filter
                        // FIXME This is per frame, parameter should be independant of frame size
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &decay_rate_default, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'D': // device name
                        if ((++arg)<argc){
                            device_name = argv[arg];
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'f': // Fast slope detection
                        single_buffer = 1 ;
                        break;
                    case 'g': // guard factor (envelope overshoot)
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &decay_factor_db, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'l': // trigger level, -db
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &trigger_level_db, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                        /*
                    case 'n': // midi note number (one per channel)
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%d%c", &trigger_level_db, &bidon) != 1) {
                                fprintf(stderr, "%s: not an integer\n", argv[arg]);
                            }
                        }else{
                            fprintf(stderr, "%s: missing value\n", argv[--arg]);
                        }
                        break;
                        */
                    case 't': // (re-)trigger delay in milliseconds
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &trig_delay_ms, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
#ifndef method1
                    case 'w': // re-trigger inhibit wait delay in milliseconds
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &wait_delay_ms, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
#endif             
                    case 'x': // Extinction (note-off) delay in milliseconds
                        if ((++arg)<argc){
                            if (sscanf(argv[arg], "%f%c", &max_note_off_delay_ms, &bidon) != 1) {
                                fprintf(stderr, "%s: not a float.\n", argv[arg]);
                                errcount++;
                            }
                        }else{
                            fprintf(stderr, "%s: missing value.\n", argv[--arg]);
                            errcount++;
                        }
                        break;
                    case 'X': // Force extinction (note-off) before re-triggering
                        force_note_off = 1;
                        break;
                    default:
                    fprintf(stderr, "%s: unknown option.\n", argv[arg]);
                    errcount++;
                }
            }else{
                fprintf(stderr, "%s: unknown option.\n", argv[arg]);
                errcount++;
            }
        }
        arg++;
    }
    
    if(errcount){
        usage(argv[0]);
        fprintf(stderr, "Aborting.\n");
        exit(-1);
    }

    // Prepare audio device for input
    if ((err = snd_pcm_open (&capture_handle, device_name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf (stderr, "cannot open audio device %s (%s)\n", 
             device_name,
             snd_strerror(err));
        exit (1);
    }else{
        printf ("audio device set to %s\n", device_name);
    }
       
    if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
        fprintf (stderr, "cannot allocate hardware parameter structure (%s)\n",
             snd_strerror(err));
        exit (1);
    }
             
    if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) {
        fprintf (stderr, "cannot initialize hardware parameter structure (%s)\n",
             snd_strerror(err));
        exit (1);
    }

    if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf (stderr, "cannot set access type (%s)\n",
             snd_strerror(err));
        exit (1);
    }

    if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, SND_PCM_FORMAT_S24_3LE)) < 0) {
        if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
            fprintf (stderr, "cannot set sample format (%s)\n",
                 snd_strerror(err));
            fprintf (stderr, "use arecord to test your soundcard input format(s)\n");
            exit (1);
        }else{
            channel_bytes = 2;
            max_sample_value = 0x7FFF;
            f = find_peak_S16_LE;
            f_peak = find_channel_peak_S16_LE;
            f_trig = find_channel_trig_S16_LE;
            printf ("sample format set to S16_LE\n");
        }
    }else{
        channel_bytes = 3;
        max_sample_value = 0x7FFFFF;
        f = find_peak_S24_3LE;
        f_peak = find_channel_peak_S24_3LE;
        f_trig = find_channel_trig_S24_3LE;
        printf ("sample format set to S24_3LE\n");
    }
    frame_bytes = channels * channel_bytes;
    buf_bytes = buf_frames * frame_bytes;
    buf = malloc(buf_bytes);

    if ((err = snd_pcm_hw_params_set_rate_near (capture_handle, hw_params, &sample_rate, 0)) < 0) {
        // Was 44100, caused a segfault
        fprintf (stderr, "cannot set sample rate to %u (%s)\n", sample_rate,
             snd_strerror(err));
        exit (1);
    }else{
        fprintf (stderr, "sample rate set to %u\n", sample_rate);
    }

    if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, channels)) < 0) {
        fprintf (stderr, "cannot set channel count to %u (%s)\n", channels,
             snd_strerror(err));
        exit (1);
    }else{
        fprintf (stderr, "channel count set to %u\n", channels);
    }

    if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) {
        fprintf (stderr, "cannot set parameters (%s)\n",
             snd_strerror(err));
        exit (1);
    }else{
        fprintf (stderr, "hardware parameters set\n");
    }

    snd_pcm_hw_params_free (hw_params);

    if ((err = snd_pcm_prepare (capture_handle)) < 0) {
        fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
             snd_strerror(err));
        exit (1);
    }else{
        fprintf (stderr, "audio interface prepared for use\n");
    }
    
    err = snd_rawmidi_open(NULL, &handle_out, "virtual", 0);
    if (err) {
        fprintf(stderr,"snd_rawmidi_open failed: %d\n", err);
        exit (1); // Unclean
    }

    signal(SIGINT, intHandler);

    // Tested values ok for 128 frames:
    // trig_delay_buffers = 4, decay_rate = 0.98, decay_factor = 2.0
    // 4 x 128 frames at 44100 Hz = 11.6 ms
    // T = 1/ln(0.98) = 49 buffers
    int waiting[channels], trig_delay_buffers[channels]; // Used for de-bouncing
    int trig_level[channels];
    unsigned int max_note_off_delay_bufs;
    int note_off_delay[channels];
    int midi_channel[channels], midi_note[channels];

#ifdef meth1            
     // Method 1 specific
    float decay[channels], decay_rate[channels], decay_factor[channels];
    int rising[channels];
    // These can be used for slope detection
    //~ int previous[channels];
    //~ int max_d[channels];
    int previous_max_l[channels], previous_previous_max_l[channels];
    int previous_max_v[channels], previous_previous_max_v[channels];
    int max_l[channels];
    decay_factor_default = exp(decay_factor_db* log(2)/6.0);
    printf("decay initial factor %f db, value %f\n", decay_factor_db, decay_factor_default);
    printf("decay per buffer: %f\n", decay_rate_default);
#else    
    // Method 2 specific
    typedef enum {
        STATE_IDLE, // Until trig level reached
        STATE_PEAK, // Scan for peak until time elapsed
        STATE_WAIT, // Inhibit retrigger until time2 elapsed
        STATE_UNKNOWN // Only for init
	} State;
	const char * state_names[] = {"IDLE", "PEAK", "WAIT"};
	State state[channels];
    State old_state[channels];
	int peak_frames[channels], wait_frames[channels], frame_count[channels];
	int peak_level[channels];// , trig_frame[channels];
#endif
	
    float ms_per_buffer;
    long int bufcount=0;
    
    frame_bytes = channel_bytes * channels;
    ms_per_buffer = (buf_frames * 1000.0)/(float)sample_rate;
    trig_delay_frames_default = roundf(trig_delay_ms * sample_rate) / 1000;
    trig_delay_buffers_default = trig_delay_frames_default / buf_frames;
    wait_delay_frames_default = roundf(wait_delay_ms * sample_rate) / 1000;
    wait_delay_buffers_default = wait_delay_frames_default / buf_frames;
    trig_level_default = max_sample_value / exp(trigger_level_db * log(2)/-6.0); // FIXME must check >0 !!
    printf("trigger level %f db factor %u, value %u\n", trigger_level_db, (int)(exp(trigger_level_db * log(2)/-6.0)), trig_level_default);

    max_note_off_delay_bufs = (unsigned int )(max_note_off_delay_ms / ms_per_buffer);
    printf("note off delay %u buffers (%f ms)\n", max_note_off_delay_bufs, max_note_off_delay_bufs * ms_per_buffer);

    printf("buffer length: %u frames (%u bytes)\n", buf_frames, buf_bytes);
    printf("time per buffer: %f ms\n", ms_per_buffer);
    printf("re-trigger delay (buffers): %u (%f ms)\n",
        trig_delay_buffers_default,
        trig_delay_buffers_default * ms_per_buffer
        );
    printf("re-trigger delay (frames): %u (%f ms)\n",
        trig_delay_frames_default,
        (float)trig_delay_frames_default * 1000 / sample_rate
        );

    int c;
    for(c = 0; c < channels; c++){
        // Parameters
        // FIXME set through command line or other (config file? OSC? midi in?)
        // FIXME make parameter decay independant of frame size and sample rate
        // FIXME use sensible units
        // FIXME should be a struct
        trig_level[c] = trig_level_default;
		printf("channel %u trigger level %u\n", c, trig_level[c]);
#ifdef meth1            
        trig_delay_buffers[c] = trig_delay_buffers_default;
        decay_rate[c] = decay_rate_default;
        decay_factor[c] = decay_factor_default; // Should this depend on sample #?
        // State variables
        rising[c] = 0; // Not rising
        decay[c] = 0.0;
        waiting[c] = 0; // Not waiting
        max_l[c] = 0;
        previous_max_l[c] = 0;
        previous_previous_max_l[c] = 0;
#else
		wait_frames[c] = wait_delay_frames_default;
		peak_frames[c] = trig_delay_frames_default;
		printf("channel %u peak window %u frames retrigger inhibit %u frames\n", c, peak_frames[c], wait_frames[c]);
        state[c]=STATE_IDLE;
        old_state[c]=STATE_UNKNOWN;
#endif
        midi_channel[c] = c & 0x0F; // Default, midi output channels map 1:1 to soundcard inputs
        midi_note[c] = 60;
        note_off_delay[c] = 0; // No pending note
        //~ previous[c] = 0;
        //~ max_d[c] = 0;
        
    }
    
    ///////////////
    // Main loop //
    ///////////////
    printf ("About to start reading\n");
    while (keepRunning) { 
        if ((err = snd_pcm_readi (capture_handle, buf, buf_frames)) != buf_frames) {
            fprintf (stderr, "read from audio interface failed (%s)\n",
                 snd_strerror(err));
            keepRunning = 0;
            //~ exit (1);
        }else{ // Audio read success
            bufcount++;
#ifdef debug
            fprintf (stderr, ".");
#endif
#ifdef meth1            
            for(c = 0; c < channels; c++){
                previous_previous_max_l[c] = previous_max_l[c];
                previous_max_l[c] = max_l[c];
                max_l[c] = 0; // l for level (always positive)
                previous_previous_max_v[c] = previous_max_v[c];
                //~ max_d[c] = 0; // d for difference (always positive) // FIXME use previous[c]
            }
            // Format-dependant peak detection
            (*f)(channels, buf, max_l, previous_max_l, previous_max_v);//, previous_previous_max_l, previous_previous_max_v);

            // React to peak in current, previous and before previous buffer
            // Will wait actual decay before sending, i.e. max_l < previous_max_l
            // test showed max rising for 4 buffers at 44100Hz, 64 frames per buffer (~6ms)
            // 6ms max reaction time should be ok when playing
            for(c = 0; c < channels; c++){
                if (rising[c]){ // Trigger detected in previous buffer
                    rising[c]++; // For stats; should we set a limit?
#ifdef debug
                    //~ fprintf (stderr, "r");
#endif
                    // Requiring 2 consecutive falling buffers can audibly increase latency
                    if ((max_l[c] < previous_max_l[c]) && (single_buffer || (previous_max_l[c] < previous_previous_max_l[c]))){
#ifdef debug
                        fprintf (stderr, "f %u ", rising[c]);
#endif
                        rising[c] = 0; // no longer rising
                        waiting[c] = trig_delay_buffers[c]; // Start or restart wait period
                        //~ decay[c] = (float)(previous_max_l[c] - trig_level[c]) * decay_factor[c]; // ... and envelope
                        decay[c] = (float)(previous_previous_max_l[c] - trig_level[c]) * decay_factor[c]; // ... and envelope
                        // Prepare to send a note off after a certain number of frames
                        // could make it depend on hit strength?
#ifdef debug
                        fprintf (stderr, "\nI %u %u %lu ", c, note_off_delay[c], bufcount);
#endif
                        if(force_note_off && note_off_delay[c]){
                            send_note_off(midi_channel[c], midi_note[c]);
#ifdef debug
                            fprintf (stderr, "\nX %u %u %lu ", c, note_off_delay[c], bufcount);
#endif
                        }
                        note_off_delay[c] = max_note_off_delay_bufs;
                        //~ send_note_on(midi_channel[c], midi_note[c], previous_max_v[c]);
                        send_note_on(midi_channel[c], midi_note[c], previous_previous_max_v[c]);
#ifdef debug
                        fprintf (stderr, "\n! %u %d %lu ", c, previous_previous_max_v[c], bufcount);
#endif
                    }
                }else if (waiting[c]){
                    waiting[c]--;
#ifdef debug
                    fprintf (stderr, "w %u", c);
#endif
                }else{ // Decaying, ready for trigger
                    if (max_l[c] > (trig_level[c] + decay[c])){ // Trigger found in this buffer
                        rising[c] = 1;
                    }
                    if (decay[c] < 1.0){
#ifdef debug
                        //~ fprintf (stderr,".");
#endif
                        decay[c] = 0.0;
                    }else{
#ifdef debug
                        //~ fprintf (stderr, "d");
                        //~ fprintf (stderr, "d %u %f\n", c, decay[c]);
#endif
                        decay[c] *= decay_rate[c];
                    }
                }
                //~ fprintf (stderr, ". %u %u", c, note_off_delay[c]);
                // Note off handling
                if (note_off_delay[c]){
                    note_off_delay[c]--;
#ifdef debug
                    fprintf (stderr, "n %u %u ", c, note_off_delay[c]);
#endif
                    if (note_off_delay[c] <= 0){
                        note_off_delay[c] = 0;
#ifdef debug
                        fprintf (stderr, "\nx %u %lu ", c, bufcount);
#endif
                        send_note_off(midi_channel[c], midi_note[c]);
                    }
                }
            } // End of loop for channels, method 1
#else // method 2
// Looking for peak can span multiple buffers
// timing  should be sample-accurate but midi isn't!
            int remaining_frames; // Remaining in current buffer
            int trig_frame, peak_frame, span;
            int velocity;
            char * buf_tail;
            for(c = 0; c < channels; c++){
				// Should have a loop to handle tail of buffer
				remaining_frames = buf_frames;
				buf_tail = buf;
				// Sate will not necessarily extend to end of buffer,
				// we need to loop over buffer chunks.
				while (remaining_frames>0) {
#ifdef debug					
					if (state[c]!=old_state[c]){
					    // fprintf (stderr, "%c %u %u->", state_names[old_state[c]][0], c, remaining_frames);
					    fprintf (stderr, "%c %u %u ", state_names[state[c]][0], c, remaining_frames);
					    old_state[c]=state[c];
					}
#endif					
					switch (state[c]){
						case STATE_IDLE:
							// Look if trigger level is reached
							trig_frame=(*f_trig)(channels, buf_tail, remaining_frames, c, trig_level[c]);
							if (trig_frame>=0){  // Trigger level was reached
								buf_tail += frame_bytes * (trig_frame+1);
								remaining_frames -= trig_frame+1;
#ifdef debug
								fprintf (stderr, "t%u r%u ", trig_frame, remaining_frames);
#endif								
								// prepare for next stage
								state[c] = STATE_PEAK;
								peak_level[c] = trig_level[c];
								frame_count[c] = peak_frames[c];
							}else{ // Trigger level was not reached in this buffer
								remaining_frames = 0; // Maybe in next buffer...
								// State stays STATE_IDLE
							}
							break;
						case STATE_PEAK:
							// look for peak within allowed time frame
							span = min(remaining_frames, frame_count[c]);
							(*f_peak)(channels, buf_tail, span, c, &peak_level[c]);
							frame_count[c] -= span;
							buf_tail += frame_bytes * span;
							remaining_frames -= span;
							if (frame_count[c]<=0){ // Is end of peak measurement window reached?
								velocity = 1+126*(peak_level[c]-trig_level[c])/(max_sample_value-trig_level[c]);
#ifdef debug								
								fprintf (stderr, "p:%u v:%u\n", peak_level[c], velocity);
#endif								
								// Send MIDI note
								send_note_on(midi_channel[c], midi_note[c], velocity);
								state[c] = STATE_WAIT;
								// FIXME should be from actual peak frame
								// but this is not necessarily in the current buffer
								frame_count[c] += wait_frames[c];
								note_off_delay[c] = max_note_off_delay_bufs;
#ifdef debug
							}else{
								fprintf (stderr, "p");
#endif
							}
							break;
						default: // case STATE_WAIT:
							frame_count[c] -= buf_frames;
							// do nothing until retrigger guard is reached
							if (frame_count[c]<=0){
								// frame_count[c]+=buf_frames;
								state[c] = STATE_IDLE;
								remaining_frames = -frame_count[c];
								buf_tail = buf + frame_bytes * (buf_frames - remaining_frames);
							}else{ // Wait for whole buffer duration
#ifdef debug
								fprintf (stderr, "w");
#endif
								remaining_frames = 0;
							}
							break;
					} // End of switch
				} // End of while buffer chunk loop
                if (note_off_delay[c]){
                    note_off_delay[c]--;
#ifdef debug
                    fprintf (stderr, "n %u %u ", c, note_off_delay[c]);
#endif
                    if (note_off_delay[c] <= 0){
                        note_off_delay[c] = 0;
#ifdef debug
                        fprintf (stderr, "\nx %u %lu ", c, bufcount);
#endif
                        send_note_off(midi_channel[c], midi_note[c]);
                    }
                }
			} // End of loop for channels, method 2
#endif
        } // end of else read success
    } // end of main read loop

    printf ("Terminating...\n");
    snd_pcm_close(capture_handle);
    if (handle_out) {
            snd_rawmidi_drain(handle_out); 
            snd_rawmidi_close(handle_out);  
    }
    if (buf) free(buf);
    exit (0);
}

/* Sample output
bonap@jack:/mnt/wd2tbk2p1/mp/mp_source/tap2midi$ ./tap2midi -D hw:0,0 -d 0.98 -t 8 -l -24
audio device set to hw:0,0
sample format set to S16_LE
sample rate set to 44100
channel count set to 2
hardware parameters set
audio interface prepared for use
decay initial factor 6.000000 db, value 2.000000
note off delay 86 buffers (249.614517 ms)
re-trigger delay (buffers): 2 (5.804989 ms)
buffer length: 128 frames (512 bytes)
time per buffer: 2.902494 ms
decay per buffer: 0.980000
trigger level factor 15, value 2047
About to start reading
.I 0 128 I 1 128 ...............................................................................................^CInterrupted!
*/
