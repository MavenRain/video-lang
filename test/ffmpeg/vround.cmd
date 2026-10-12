ffmpeg -i vfr5.mp4 -vf trim=start_frame=20:end_frame=41,setpts=PTS-STARTPTS -af atrim=start_sample=35280:end_sample=94065,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
