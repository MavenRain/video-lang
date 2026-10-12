ffmpeg -i vfr.mp4 -vf setpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
