ffmpeg -i g12.mp4 -vf trim=start_frame=24:end_frame=48,setpts=PTS-STARTPTS -af atrim=start_sample=42336:end_sample=84672,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
