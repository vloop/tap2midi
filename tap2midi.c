// Tap 2 MIDI
// Marc Perilleux 2019
// Detect taps on microphone or other sound input and triggers MIDI notes (velocity-sensitive)

// Includes code from http://equalarea.com/paul/alsa-audio.html Minimal Capture Program
// Compile with:
// gcc tap2midi.c -lasound -lm -o tap2midi
// Use example (maybe a bit conservative):
// wait time 8ms, trigger level -24 db
// ./tap2midi hw:3,0 -d 0.98 -t 8 -l -24
// A little more responsive:
// -d 0.98 -t 0 -l -30
// NB - to identify your soundcard (hw:3,0 above), use
// arecord -l

// Currently hard-coded to S24_3LE sample format
// int must be at least 32 bits

// Debouncing uses 2 different mechanisms, which can be combined:
// - set a certain delay time before retriggering is allowed
//   use parameter -t followed by milliseconds
// - trigger only if level exceeds a decreasing envelope
//   use parameter -d followed by decay rate per buffer FIXME
//   and parameter -g guard factor (envelope overshoot) FIXME

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

#define buf_frames (64)
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
    printf("-t time     retrigger delay time (ms)\n");
    printf("            typically 0, higher values mean more anti-bouncing\n");
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
    float trig_delay_ms = 0;
    int trig_delay_frames_default;
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
            printf ("sample format set to S16_LE\n");
        }
    }else{
        channel_bytes = 3;
        max_sample_value = 0x7FFFFF;
        f = find_peak_S24_3LE;
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
    // trig_delay_frames = 4, decay_rate = 0.98, decay_factor = 2.0
    // 4 x 128 frames at 44100 Hz = 11.6 ms
    // T = 1/ln(0.98) = 49 buffers
    int waiting[channels], trig_delay_frames[channels]; // Used for de-bouncing
    int trig_level[channels];
    float decay[channels], decay_rate[channels], decay_factor[channels];
    unsigned int max_note_off_delay_bufs;
    int note_off_delay[channels];
    int midi_channel[channels], midi_note[channels];
    int rising[channels];
    // These can be used for slope detection
    //~ int previous[channels];
    //~ int max_d[channels];
    int previous_max_l[channels], previous_previous_max_l[channels];
    int previous_max_v[channels], previous_previous_max_v[channels];
    int max_l[channels];
    float ms_per_buffer;
    long int bufcount=0;

    ms_per_buffer = (buf_frames * 1000.0)/(float)sample_rate;
    trig_delay_frames_default = roundf(trig_delay_ms * sample_rate) / buf_frames / 1000;
    trig_level_default = max_sample_value / exp(trigger_level_db * log(2)/-6.0); // FIXME must check >0 !!

    decay_factor_default = exp(decay_factor_db* log(2)/6.0);
    printf("decay initial factor %f db, value %f\n", decay_factor_db, decay_factor_default);

    max_note_off_delay_bufs = (unsigned int )(max_note_off_delay_ms / ms_per_buffer);
    printf("note off delay %u buffers (%f ms)\n", max_note_off_delay_bufs, max_note_off_delay_bufs * ms_per_buffer);

    printf("re-trigger delay (buffers): %lu (%f ms)\n",
        trig_delay_frames_default,
        trig_delay_frames_default * ms_per_buffer
        );
    printf("buffer length: %u frames (%u bytes)\n", buf_frames, buf_bytes);
    printf("time per buffer: %f ms\n", ms_per_buffer);
    printf("decay per buffer: %f\n", decay_rate_default);
    printf("trigger level factor %lu, value %lu\n", (int)(exp(trigger_level_db * log(2)/-6.0)), trig_level_default);

    int c;
    for(c = 0; c < channels; c++){
        // Parameters
        // FIXME set through command line or other (config file? OSC? midi in?)
        // FIXME make parameter decay independant of frame size and sample rate
        // FIXME use sensible units
        // FIXME should be a struct
        trig_delay_frames[c] = trig_delay_frames_default;
        trig_level[c] = trig_level_default;
        decay_rate[c] = decay_rate_default;
        decay_factor[c] = decay_factor_default; // Should this depend on sample #?
        midi_channel[c] = c & 0x0F; // Default, midi output channels map 1:1 to soundcard inputs
        midi_note[c] = 60;
        // State variables
        rising[c] = 0; // Not rising
        decay[c] = 0.0;
        waiting[c] = 0; // Not waiting
        note_off_delay[c] = 0; // No pending note
        max_l[c] = 0;
        previous_max_l[c] = 0;
        previous_previous_max_l[c] = 0;
        //~ previous[c] = 0;
        //~ max_d[c] = 0;
    }
    printf ("About to start reading\n");

    while (keepRunning) { 
        if ((err = snd_pcm_readi (capture_handle, buf, buf_frames)) != buf_frames) {
            fprintf (stderr, "read from audio interface failed (%s)\n",
                 snd_strerror(err));
            keepRunning = 0;
            //~ exit (1);
        }else{
            bufcount++;
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
                        waiting[c] = trig_delay_frames[c]; // Start or restart wait period
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
            }
        }
    }

    printf ("Terminating...\n");
    snd_pcm_close(capture_handle);
    if (handle_out) {
            snd_rawmidi_drain(handle_out); 
            snd_rawmidi_close(handle_out);  
    }
    if (buf) free(buf);

    exit (0);
}

