ffmpeg -i vfr5.mp4 -vf setpts=PTS-STARTPTS -af asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
