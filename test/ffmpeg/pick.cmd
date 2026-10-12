ffmpeg -i v.mp4 -vf trim=start_frame=5:end_frame=10,setpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
