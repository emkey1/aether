`test-tone-48k-s16le-stereo.raw` is a 1-second 440 Hz stereo PCM sample for `/dev/dsp`.

Guest playback test:

```sh
cat /tests/audio/test-tone-48k-s16le-stereo.raw > /dev/dsp
```

`test-tone-48k-s16le-stereo.wav` contains the same audio in a WAV container for quick inspection outside the guest.
