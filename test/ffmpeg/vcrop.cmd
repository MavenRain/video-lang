ffmpeg -i vfr2.mp4 -vf trim=start_frame=20:end_frame=41,setpts=PTS-STARTPTS,crop=160:120:40:20 -af atrim=start_sample=35280:end_sample=93492,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
