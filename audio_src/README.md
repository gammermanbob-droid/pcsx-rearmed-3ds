# Audio/video source clips

Raw signed-16-bit little-endian PCM, generated once via `ffmpeg` and
checked in directly -- NDSP wants raw PCM regardless, and neither clip
needs re-encoding at runtime, so there's no reason to ship an MP3/AAC
decoder in the app just to produce the same bytes on every boot.

Regenerate with:

```sh
# Boot chime -- full clip, audio only, downmixed 5.1->stereo, native
# 44.1kHz kept since it's short.
ffmpeg -i "PlayStation Intro 1080p [Remastered].mp4" \
  -vn -ac 2 -ar 44100 -f s16le -acodec pcm_s16le boot_chime.pcm

# Menu music -- first 45s only (it loops; the source track is 2 minutes
# and this is background music, not something worth the extra ~17MB),
# downsampled to 32kHz since fidelity matters less for a loop.
ffmpeg -i "Offical UK PlayStation Magazine Demo Disk Music_ Menu.mp3" \
  -t 45 -ac 2 -ar 32000 -f s16le -acodec pcm_s16le menu_music.pcm

# Boot intro video (New3DS only, see intro_video.c) -- a raw Annex-B
# H.264 elementary stream the console's own MVD hardware decoder can
# play back directly. No container, no audio (boot_chime.pcm above,
# extracted from this same source file, is the soundtrack). Scaled
# down to 400x224 (fits the top screen's 400px width with an even,
# macroblock-friendly height) and re-encoded at Constrained Baseline
# profile with no B-frames -- MVD is a fixed-function decoder, not a
# general one, and this keeps it well inside what's known to work
# (matching how the New3DS Internet Browser itself uses this
# hardware). 20fps rather than the source's 60 since intro_video.c
# holds each decoded picture for a fixed number of vsync ticks to pace
# playback, and there's no reason to decode 3x the pictures a mostly-
# static logo animation needs.
ffmpeg -i "PlayStation Intro 1080p [Remastered].mp4" \
  -an -vf "scale=400:224" -r 20 -c:v libx264 -profile:v baseline -level 3.0 \
  -bf 0 -g 40 -sc_threshold 0 -pix_fmt yuv420p -crf 26 \
  -bsf:v h264_mp4toannexb -f h264 intro_video.h264
```
