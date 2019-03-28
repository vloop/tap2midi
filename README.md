# Tap 2 MIDI

Detect taps on microphone or other sound input and triggers MIDI notes (velocity-sensitive)

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
Launch the program
```
./tap2midi hw:3,0 -d 0.97 -t 0 -l -36 -c 2 -g 0 -v
```

## Authors

* **Marc Perilleux** - *Initial work*

## License

This project is licensed under the 

## Acknowledgments

* Includes code from http://equalarea.com/paul/alsa-audio.html Minimal Capture Program

