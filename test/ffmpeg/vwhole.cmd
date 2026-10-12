ffmpeg -i vfr2.mp4 -vf setpts=PTS-STARTPTS -af asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
