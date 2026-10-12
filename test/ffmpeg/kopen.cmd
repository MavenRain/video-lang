ffmpeg -i g12open.mp4 -vf trim=start_frame=40:end_frame=61,setpts=PTS-STARTPTS -af atrim=start_sample=70560:end_sample=107604,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
