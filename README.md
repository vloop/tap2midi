# Tap 2 MIDI

Detect taps on microphone or other sound input and trigger MIDI notes (velocity-sensitive)

### Prerequisites

Requires libasound (ALSA) development files

```
apt install libasound2-dev
```

### Installing

Compile tap2midi.c
```
gcc tap2midi.c -lasound -lm -o tap2midi
```
You may want to copy `tap2midi` somewhere on your path.


## Running the program

Identify the soundcard you want to use with
```
arecord -l
```
You may have to enable the microphone input, for example using `alsamixer`


Launch the program
```
./tap2midi -D hw:3,0 -d 0.97 -t 0 -l -36 -c 2 -g 0 -v
```
You need to adjust the parameters to match your mic, soundcard and playing style.
```
-c channels channel count

-d rate     envelope decay rate (per buffer)

            typically 0.97..0.99, higher values mean more anti-bouncing

-D device   alsa sound input device

-f          faster slope detection (may cause double-triggering)

-g factor   initial gain of envelope (db)

            typically 0, higher values mean more anti-bouncing

-h          display this help message

-l level    trigger level (db, must be negative)

            typically -36..-24, more negative values mean more sensitivity

-r rate     sample rate (Hz)

-t time     retrigger delay time (ms)

            typically 0, higher values mean more anti-bouncing

-v          verbose

-x time     note off (extinction) delay time (ms)

-X          force note off (extinction) before new note
```


## Authors

* **Marc Perilleux** - *Initial work*

## License

This project is licensed under the 

## Acknowledgments

* Includes code from http://equalarea.com/paul/alsa-audio.html Minimal Capture Program
* Thanks to D.J.A.Y. for testing and suggesting some features
