ffmpeg -i v.mp4 -vf trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS ref.mp4
