ffmpeg -i vfr.mp4 -vf trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
