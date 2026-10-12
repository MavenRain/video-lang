ffmpeg -i vfr2.mp4 -vf trim=start_frame=20:end_frame=31,setpts=PTS-STARTPTS -af atrim=start_sample=35280:end_sample=58212,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
