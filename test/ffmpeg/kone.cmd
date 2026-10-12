ffmpeg -i g12.mp4 -vf trim=start_frame=37:end_frame=38,setpts=PTS-STARTPTS -af atrim=start_sample=65268:end_sample=67032,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
