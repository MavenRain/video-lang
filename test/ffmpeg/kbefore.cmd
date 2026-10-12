ffmpeg -i g12.mp4 -vf trim=start_frame=10:end_frame=31,setpts=PTS-STARTPTS -af atrim=start_sample=17640:end_sample=54684,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
