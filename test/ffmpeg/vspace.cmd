ffmpeg -i v.mp4 -vf trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS,scale=160:120 -fps_mode passthrough ref.mp4
