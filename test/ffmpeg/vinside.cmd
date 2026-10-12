ffmpeg -i vfr2.mp4 -vf trim=start_frame=30:end_frame=52,setpts=PTS-STARTPTS -af atrim=start_sample=54684,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
