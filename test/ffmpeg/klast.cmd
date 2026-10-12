ffmpeg -i g12.mp4 -vf trim=start_frame=50:end_frame=100,setpts=PTS-STARTPTS -af atrim=start_sample=88200,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
